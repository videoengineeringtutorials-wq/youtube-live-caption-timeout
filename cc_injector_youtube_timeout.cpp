// YouTube Live encoder with a ten-second UDP caption inactivity timeout.

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <sstream>
#include <memory>

// POSIX
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
}

#include "captions_608.h"
#include "media_filter.h"

static std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");
static void stop_stream(int) { interrupted.store(true, std::memory_order_relaxed); }
static int interrupt_input(void*) { return interrupted != 0; }

#include "packet_reader.h"
#include "flv_pacer.h"
#include "youtube_logging.h"

// ======================================================================================
// UDP
// ======================================================================================

static int open_udp(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::runtime_error("caption socket: " + std::string(strerror(errno)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        std::string error = strerror(errno);
        close(fd);
        throw std::runtime_error("caption UDP listener: " + error);
    }

    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, (sockaddr*)&addr, &addrlen) == 0) port = ntohs(addr.sin_port);
    std::cerr << "[cc] UDP :" << port << "\n";
    return fd;
}

static bool recv_line(int fd, std::string& out)
{
    char buf[512];
    int n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return false;

    buf[n] = 0;
    out.assign(buf, n); // wrapping/two-line handling happens in set_caption
    return true;
}

// ======================================================================================
// MAIN
// ======================================================================================

static void check(int result, const char* operation)
{
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}

int main(int argc, char** argv)
{
    std::string inUrl = "udp://127.0.0.1:5000";
    std::string outUrl;
    std::string youtube_server;
    std::string stream_key;

    AVFormatContext* ifmt = nullptr;
    AVFormatContext* ofmt = nullptr;
    AVCodecContext* decCtx = nullptr;
    AVCodecContext* encCtx = nullptr;
    AVCodecContext* adecCtx = nullptr;
    AVCodecContext* aencCtx = nullptr;
    AVDictionary* input_options = nullptr;
    std::unique_ptr<PacketReader> reader;
    FLVPacer flv_pacer;
    AVPacket* pkt = nullptr;
    AVPacket* opkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* filtered = nullptr;
    AVFrame* aframe = nullptr;
    AVFrame* afiltered = nullptr;
    MediaFilter video_filter;
    MediaFilter audio_filter;
    int udp = -1;
    int status = 0;

    try {
        int cc_port = 54001;
        CaptionMode caption_mode = CaptionMode::RollUp;
        bool realtime = true;
        std::string pacing = "auto";
        std::string encoder_preset = "veryfast";
        int video_bitrate = 14000000;
        std::string input_format;
        int positional = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) throw std::runtime_error("missing value for " + arg);
                return argv[i];
            };
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: " << argv[0] << " -i INPUT -o RTMPS_URL [options]\n"
                          << "       " << argv[0] << " -i INPUT --stream-key KEY [--youtube-server=SERVER]\n"
                          << "       " << argv[0] << " -i INPUT -o local-preview.flv --pace=off\n"
                          << "Output: FLV / H.264 High 1920x1080p30, CBR 14 Mbps, 2-second closed GOP; AAC stereo 44.1 kHz 128 kbps.\n"
                          << "Captions: clear displayed and hidden buffers after 10 seconds without caption data.\n"
                          << "Options: --cc-mode=rollup|popon, --cc-port=PORT (default 54001),\n"
                          << "         --video-bitrate=BPS, --preset=NAME, --pace=auto|on|off,\n"
                          << "         --input-option=KEY=VALUE (repeatable), -f INPUT_FORMAT\n"
                          << "--stream-key KEY or --stream-key=KEY overrides YOUTUBE_STREAM_KEY.\n"
                          << "Server: --youtube-server=SERVER, then YOUTUBE_STREAM_URL, then rtmps://a.rtmps.youtube.com/live2.\n";
                return 0;
            } else if (arg == "-i") { inUrl = value(); positional = 1; }
            else if (arg == "-o") outUrl = value();
            else if (arg == "--stream-key" || arg.rfind("--stream-key=", 0) == 0) {
                stream_key = arg == "--stream-key" ? value() : arg.substr(13);
                if (stream_key.empty() || stream_key[0]=='-' || stream_key.find_first_of("/\\?&# \t\r\n")!=std::string::npos)
                    throw std::runtime_error("--stream-key requires a nonempty key without URL separators or whitespace");
            }
            else if (arg == "-f" || arg == "--input-format") input_format = value();
            else if (arg == "-re" || arg == "--realtime") realtime = true;
            else if (arg.rfind("--youtube-server=", 0) == 0) youtube_server = arg.substr(17);
            else if (arg.rfind("--video-bitrate=", 0) == 0) {
                char* end = nullptr;
                long rate = std::strtol(arg.c_str()+16,&end,10);
                if (end==arg.c_str()+16 || *end || rate<1000000 || rate>25000000)
                    throw std::runtime_error("video bitrate must be 1000000..25000000 bits/second");
                video_bitrate=static_cast<int>(rate);
            }
            else if (arg.rfind("--pace=", 0) == 0) {
                pacing = arg.substr(7);
                if (pacing != "auto" && pacing != "on" && pacing != "off")
                    throw std::runtime_error("use --pace=auto|on|off");
            } else if (arg.rfind("--preset=", 0) == 0) encoder_preset = arg.substr(9);
            else if (arg == "--cc-mode=rollup") caption_mode = CaptionMode::RollUp;
            else if (arg == "--cc-mode=popon") caption_mode = CaptionMode::PopOn;
            else if (arg.rfind("--input-option=", 0) == 0) {
                std::string option = arg.substr(15);
                size_t eq = option.find('=');
                if (eq == std::string::npos || eq == 0)
                    throw std::runtime_error("use --input-option=KEY=VALUE");
                check(av_dict_set(&input_options, option.substr(0, eq).c_str(), option.substr(eq + 1).c_str(), 0), "set input option");
            } else if (arg.rfind("--cc-port=", 0) == 0) {
                char* end = nullptr;
                long port = std::strtol(argv[i] + 10, &end, 10);
                if (end == argv[i] + 10 || *end || port < 0 || port > 65535)
                    throw std::runtime_error("caption port must be between 0 and 65535");
                cc_port = static_cast<int>(port);
            } else if (!arg.empty() && arg[0] != '-' && positional < 2) {
                if (positional++ == 0 && inUrl == "udp://127.0.0.1:5000") inUrl = arg;
                else outUrl = arg;
            } else throw std::runtime_error("unknown option; see --help");
        }
        const char* env_server=std::getenv("YOUTUBE_STREAM_URL");
        const char* env_key=std::getenv("YOUTUBE_STREAM_KEY");
        if (!outUrl.empty() && !stream_key.empty())
            throw std::runtime_error("choose -o OUTPUT or --stream-key, not both");
        if (stream_key.empty() && env_key) stream_key=env_key;
        if (youtube_server.empty() && outUrl.empty() && env_server) youtube_server=env_server;
        if (!outUrl.empty() && !youtube_server.empty())
            throw std::runtime_error("choose -o or --youtube-server/environment destination, not both");
        if (outUrl.empty()) {
            if (stream_key.empty())
                throw std::runtime_error("provide -o OUTPUT, --stream-key KEY, or YOUTUBE_STREAM_KEY");
            if (youtube_server.empty()) youtube_server="rtmps://a.rtmps.youtube.com/live2";
            while (!youtube_server.empty() && youtube_server.back()=='/') youtube_server.pop_back();
            outUrl=youtube_server+"/"+stream_key;
        }
        bool network_output=outUrl.rfind("rtmp://",0)==0 || outUrl.rfind("rtmps://",0)==0;
        if (!network_output && outUrl.find("://")!=std::string::npos)
            throw std::runtime_error("YouTube output must be RTMP(S); use a local .flv path for preview");
        if (!network_output && (outUrl.size()<4 || outUrl.substr(outUrl.size()-4)!=".flv"))
            throw std::runtime_error("local preview output must end in .flv");
        if (network_output) {
            youtube_secrets.push_back(outUrl);
            size_t slash=outUrl.rfind('/');
            youtube_secrets.push_back(outUrl.substr(slash+1));
        }
        if (env_key && *env_key) youtube_secrets.push_back(env_key);
        if (!stream_key.empty()) youtube_secrets.push_back(stream_key);
        av_log_set_callback(youtube_log);
        av_log_set_level(AV_LOG_WARNING);
        if (pacing=="off") realtime=false;
        else if (pacing=="on") realtime=true;
        else realtime=network_output;
        std::cerr << "[output] timestamp pacing " << (realtime ? "enabled" : "disabled") << "\n";
        std::signal(SIGINT, stop_stream);
        std::signal(SIGTERM, stop_stream);
        check(avformat_network_init(), "network init");
        const AVInputFormat* input_demuxer = nullptr;
        if (!input_format.empty()) {
            input_demuxer = av_find_input_format(input_format.c_str());
            if (!input_demuxer) throw std::runtime_error("unknown input format: " + input_format);
        }
        ifmt = avformat_alloc_context();
        if (!ifmt) throw std::bad_alloc();
        ifmt->interrupt_callback = AVIOInterruptCB{interrupt_input, nullptr};
        ifmt->flags |= AVFMT_FLAG_GENPTS;
        check(avformat_open_input(&ifmt, inUrl.c_str(), input_demuxer, &input_options), "open input");
        if (input_options)
            throw std::runtime_error("unrecognized input option: " + std::string(av_dict_get(input_options, "", nullptr, AV_DICT_IGNORE_SUFFIX)->key));
        check(avformat_find_stream_info(ifmt, nullptr), "read stream information");
        int vIdx = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(vIdx, "find video stream");
        int aIdx = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, vIdx, nullptr, 0);
        for (unsigned i = 0; i < ifmt->nb_streams; ++i)
            ifmt->streams[i]->discard = (static_cast<int>(i) == vIdx || static_cast<int>(i) == aIdx)
                ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        std::cerr << "[input] selected video stream " << vIdx << ", audio stream " << aIdx << "\n";
        AVStream* vin = ifmt->streams[vIdx];

        const AVCodec* dec = avcodec_find_decoder(vin->codecpar->codec_id);
        if (!dec) throw std::runtime_error("video decoder not found");
        decCtx = avcodec_alloc_context3(dec);
        if (!decCtx) throw std::bad_alloc();
        check(avcodec_parameters_to_context(decCtx, vin->codecpar), "copy decoder parameters");
        decCtx->pkt_timebase = vin->time_base;
        check(avcodec_open2(decCtx, dec, nullptr), "open video decoder");

        const AVCodec* enc = avcodec_find_encoder_by_name("libx264");
        if (!enc) throw std::runtime_error("libx264 encoder not found");
        encCtx = avcodec_alloc_context3(enc);
        if (!encCtx) throw std::bad_alloc();
        encCtx->width = 1920;
        encCtx->height = 1080;
        encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        encCtx->sample_aspect_ratio = AVRational{1, 1};
        // Keep MPEG-TS clock precision. A frame count erases dropped-frame gaps
        // and capture-clock variation, causing cumulative drift against audio.
        encCtx->time_base = AVRational{1, 90000};
        encCtx->framerate = AVRational{30, 1};
        encCtx->bit_rate = video_bitrate;
        encCtx->rc_min_rate = video_bitrate;
        encCtx->rc_max_rate = video_bitrate;
        encCtx->rc_buffer_size = 2*video_bitrate;
        encCtx->gop_size = 60;
        encCtx->max_b_frames = 2;
        encCtx->refs = 1;
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_CLOSED_GOP;
        encCtx->field_order = AV_FIELD_PROGRESSIVE;
        encCtx->color_range = AVCOL_RANGE_MPEG;
        encCtx->colorspace = AVCOL_SPC_BT709;
        encCtx->color_primaries = AVCOL_PRI_BT709;
        encCtx->color_trc = AVCOL_TRC_BT709;
        check(av_opt_set(encCtx->priv_data, "a53cc", "1", 0), "enable captions");
        check(av_opt_set(encCtx->priv_data, "preset", encoder_preset.c_str(), 0), "set encoder preset");
        check(av_opt_set(encCtx->priv_data,"profile","high",0),"set H.264 profile");
        check(av_opt_set(encCtx->priv_data,"level","4.1",0),"set H.264 level");
        // Presets such as ultrafast disable CABAC/8x8 transforms. Override them
        // so preset changes retain the advertised High profile and CABAC.
        check(av_opt_set(encCtx->priv_data,"x264-params", "nal-hrd=cbr:force-cfr=1:cabac=1:8x8dct=1:rc-lookahead=0:sync-lookahead=0:b-adapt=0:mbtree=0:open-gop=0:scenecut=0:keyint=60:min-keyint=60:ref=1:b-pyramid=none:repeat-headers=0",0),"set YouTube rate/GOP options");
        check(avcodec_open2(encCtx, enc, nullptr), "open video encoder");

        check(avformat_alloc_output_context2(&ofmt, nullptr, "flv", outUrl.c_str()), "create output");
        ofmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        ofmt->max_interleave_delta = 0; // preserve global DTS order while B-frames buffer
        ofmt->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
        check(av_opt_set(ofmt->priv_data,"flvflags","no_duration_filesize",0),"set live FLV metadata");
        int64_t origin_us=AV_NOPTS_VALUE;
        for (int index : {vIdx,aIdx}) {
            if (index>=0 && ifmt->streams[index]->start_time!=AV_NOPTS_VALUE) {
                int64_t time=av_rescale_q(ifmt->streams[index]->start_time,ifmt->streams[index]->time_base,AV_TIME_BASE_Q);
                if (origin_us==AV_NOPTS_VALUE || time<origin_us) origin_us=time;
            }
        }
        // Use one common epoch for both streams, not independent zero resets.
        if (origin_us==AV_NOPTS_VALUE) origin_us=ifmt->start_time;
        if (origin_us==AV_NOPTS_VALUE) origin_us=0;
        auto normalize_packet = [&](AVPacket* packet, AVStream* stream) {
            int64_t offset=av_rescale_q(origin_us,AV_TIME_BASE_Q,stream->time_base);
            if (packet->pts!=AV_NOPTS_VALUE) packet->pts-=offset;
            if (packet->dts!=AV_NOPTS_VALUE) packet->dts-=offset;
        };
        AVStream* vout = avformat_new_stream(ofmt, nullptr);
        if (!vout) throw std::bad_alloc();
        check(avcodec_parameters_from_context(vout->codecpar, encCtx), "copy video parameters");
        vout->time_base = encCtx->time_base;
        vout->avg_frame_rate = encCtx->framerate;

        // Always produce AAC-LC stereo; synthesize silence for video-only input.
        AVStream* aout=avformat_new_stream(ofmt,nullptr);
        if (!aout) throw std::bad_alloc();
        if (aIdx>=0) {
            AVStream* ain=ifmt->streams[aIdx];
            const AVCodec* adec=avcodec_find_decoder(ain->codecpar->codec_id);
            if (!adec) throw std::runtime_error("audio decoder not found");
            adecCtx=avcodec_alloc_context3(adec);
            if (!adecCtx) throw std::bad_alloc();
            check(avcodec_parameters_to_context(adecCtx,ain->codecpar),"copy audio decoder parameters");
            adecCtx->pkt_timebase=ain->time_base;
            check(avcodec_open2(adecCtx,adec,nullptr),"open audio decoder");
            check(av_dict_copy(&aout->metadata,ain->metadata,0),"copy audio metadata");
        }
        const AVCodec* aenc=avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aenc) throw std::runtime_error("AAC encoder not found");
        aencCtx=avcodec_alloc_context3(aenc);
        if (!aencCtx) throw std::bad_alloc();
        aencCtx->sample_rate=44100;
        aencCtx->sample_fmt=AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&aencCtx->ch_layout,2);
        aencCtx->time_base=AVRational{1,44100};
        aencCtx->bit_rate=128000;
        aencCtx->profile=FF_PROFILE_AAC_LOW;
        aencCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(aencCtx,aenc,nullptr),"open AAC encoder");
        check(avcodec_parameters_from_context(aout->codecpar,aencCtx),"copy AAC parameters");
        aout->time_base=aencCtx->time_base;
        std::cerr << "[audio] AAC-LC 44.1 kHz stereo 128 kbps" << (aIdx<0 ? " (generated silence)" : "") << "\n";

        udp = open_udp(cc_port);
        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
            AVDictionary* options=nullptr;
            check(av_dict_set(&options,"rw_timeout","10000000",0),"set output timeout");
            int opened=avio_open2(&ofmt->pb,outUrl.c_str(),AVIO_FLAG_WRITE,&ifmt->interrupt_callback,&options);
            av_dict_free(&options);
            check(opened,"open YouTube/preview output");
            if (realtime) {
                ofmt->pb=flv_pacer.attach(ofmt->pb);
                ofmt->flags |= AVFMT_FLAG_CUSTOM_IO;
            }
        }
        check(avformat_write_header(ofmt, nullptr), "write output header");

        pkt = av_packet_alloc();
        opkt = av_packet_alloc();
        frame = av_frame_alloc();
        filtered = av_frame_alloc();
        aframe = av_frame_alloc();
        afiltered = av_frame_alloc();
        if (!pkt || !opkt || !frame || !filtered || !aframe || !afiltered) throw std::bad_alloc();
        CCEncoder ccEnc{};
        ccEnc.mode = caption_mode;
        set_caption(ccEnc, "CC ONLINE");
        int64_t last_caption_us = 0;
        bool captions_active = true;
        int64_t last_pts = AV_NOPTS_VALUE;
        int64_t last_source_pts = AV_NOPTS_VALUE;
        int64_t last_audio_pts = AV_NOPTS_VALUE;
        AVRational input_rate = av_guess_frame_rate(ifmt, vin, nullptr);
        if (input_rate.num <= 0 || input_rate.den <= 0) input_rate = AVRational{30000, 1001};
        int filter_width = 0, filter_height = 0, filter_format = -1;
        AVRational filter_sar = {0, 1};
        int64_t pacing_pts = AV_NOPTS_VALUE, pacing_time = 0;
        int64_t source_frames = 0, output_frames = 0, source_gaps = 0;
        int64_t last_audio_packet_dts=AV_NOPTS_VALUE, skipped_audio=0;
        int64_t silence_pts=AV_NOPTS_VALUE;
        int64_t encode_work_us = 0;
        int64_t max_encode_us = 0, max_late_us = 0;
        int64_t stats_time = av_gettime_relative();
        auto report_stats = [&]() {
            std::cerr << "[stats] source_video=" << source_frames << " output_video=" << output_frames
                      << " source_gaps=" << source_gaps << " skipped_audio=" << skipped_audio << " max_encode_ms=" << max_encode_us / 1000.0
                      << " max_schedule_late_ms=" << std::max(max_late_us, flv_pacer.max_late_us) / 1000.0 << "\n";
            stats_time = av_gettime_relative();
        };
        const int64_t frame_duration = av_rescale_q(1, av_inv_q(encCtx->framerate), encCtx->time_base);

        auto drain_encoder = [&]() {
            int ret;
            while (true) {
                int64_t began = av_gettime_relative();
                ret = avcodec_receive_packet(encCtx, opkt);
                encode_work_us += av_gettime_relative() - began;
                if (ret < 0) break;
                // The muxer may change stream time_base when writing its header.
                av_packet_rescale_ts(opkt, encCtx->time_base, vout->time_base);
                opkt->stream_index = vout->index;
                opkt->pos = -1;
                normalize_packet(opkt, vout);
                check(av_interleaved_write_frame(ofmt, opkt), "mux video");
                av_packet_unref(opkt);
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) check(ret, "receive encoded video");
        };
        auto drain_audio_encoder = [&]() {
            int ret;
            while ((ret = avcodec_receive_packet(aencCtx, opkt)) >= 0) {
                av_packet_rescale_ts(opkt, aencCtx->time_base, aout->time_base);
                opkt->stream_index = aout->index;
                opkt->pos = -1;
                normalize_packet(opkt,aout);
                check(av_interleaved_write_frame(ofmt, opkt), "mux AAC audio");
                av_packet_unref(opkt);
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) check(ret, "receive AAC audio");
        };
        auto emit_video = [&](AVFrame* output, AVRational tb) {
            int64_t scheduled_time = 0;
            output->pts = av_rescale_q(output->pts, tb, encCtx->time_base);
            if (last_pts != AV_NOPTS_VALUE && output->pts <= last_pts)
                throw std::runtime_error("video timestamp moved backwards or repeated; restart after source discontinuity");
            last_pts = output->pts;
            if (realtime && !flv_pacer.bridge) {
                if (pacing_pts == AV_NOPTS_VALUE) {
                    pacing_pts = output->pts;
                    pacing_time = av_gettime_relative();
                }
                int64_t target = pacing_time + av_rescale_q(output->pts - pacing_pts, encCtx->time_base, AV_TIME_BASE_Q);
                scheduled_time = target;
                while (!interrupted) {
                    int64_t remaining = target - av_gettime_relative();
                    if (remaining <= 0) break;
                    av_usleep(std::min<int64_t>(50000, remaining));
                }
            }
#if LIBAVUTIL_VERSION_MAJOR >= 58
            output->duration=frame_duration;
            output->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
#else
            output->pkt_duration=frame_duration;
            output->interlaced_frame=0;
            output->top_field_first=0;
#endif
            output->pict_type = AV_PICTURE_TYPE_NONE;
            if (aIdx<0) {
                if (silence_pts==AV_NOPTS_VALUE) silence_pts=av_rescale_q(output->pts,encCtx->time_base,aencCtx->time_base);
                int64_t end=av_rescale_q(output->pts+frame_duration,encCtx->time_base,aencCtx->time_base);
                while (silence_pts<end) {
                    aframe->format=aencCtx->sample_fmt;
                    aframe->sample_rate=aencCtx->sample_rate;
                    check(av_channel_layout_copy(&aframe->ch_layout,&aencCtx->ch_layout),"set silent audio layout");
                    aframe->nb_samples=aencCtx->frame_size;
                    aframe->pts=silence_pts;
                    check(av_frame_get_buffer(aframe,0),"allocate silent audio");
                    check(av_samples_set_silence(aframe->extended_data,0,aframe->nb_samples,2,aencCtx->sample_fmt),"generate silent audio");
                    check(avcodec_send_frame(aencCtx,aframe),"encode silent audio");
                    drain_audio_encoder();
                    silence_pts+=aframe->nb_samples;
                    av_frame_unref(aframe);
                }
            }
            std::string line;
            const int64_t caption_now_us = av_gettime_relative();
            if (!last_caption_us) last_caption_us = caption_now_us;
            if (ccEnc.pending.empty()) {
                // Consume input before expiring: even a repeated caption is
                // fresh data and keeps the current screen alive.
                if (recv_line(udp, line)) {
                    set_caption(ccEnc, line);
                    last_caption_us = caption_now_us;
                    captions_active = !caption_lines(line).empty();
                } else if (captions_active && caption_now_us - last_caption_us >= 10000000) {
                    set_caption(ccEnc, "\n"); // erase displayed and hidden memories
                    captions_active = false;
                    std::cerr << "[cc] cleared after 10 seconds without caption data\n";
                }
            }
            std::vector<uint8_t> cc;
            build_cc(cc, ccEnc);
            av_frame_remove_side_data(output, AV_FRAME_DATA_A53_CC);
            AVFrameSideData* sd = av_frame_new_side_data(output, AV_FRAME_DATA_A53_CC, cc.size());
            if (!sd) throw std::bad_alloc();
            memcpy(sd->data, cc.data(), cc.size());
            int64_t encode_start = av_gettime_relative();
            check(avcodec_send_frame(encCtx, output), "encode video");
            encode_work_us = av_gettime_relative() - encode_start;
            drain_encoder();
            int64_t now = av_gettime_relative();
            max_encode_us = std::max(max_encode_us, encode_work_us);
            if (scheduled_time && !interrupted) max_late_us = std::max(max_late_us, now - scheduled_time);
            ++output_frames;
            if (now - stats_time >= 10000000) report_stats();
        };
        auto drain_video_filter = [&]() {
            int ret;
            while ((ret = av_buffersink_get_frame(video_filter.sink, filtered)) >= 0) {
                emit_video(filtered, av_buffersink_get_time_base(video_filter.sink));
                av_frame_unref(filtered);
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) check(ret, "receive converted video");
        };
        auto drain_decoder = [&]() {
            int ret;
            while ((ret = avcodec_receive_frame(decCtx, frame)) >= 0) {
                int64_t source_pts = frame->best_effort_timestamp;
                if (source_pts == AV_NOPTS_VALUE) source_pts = frame->pts;
                if (source_pts == AV_NOPTS_VALUE) {
                    if (last_source_pts == AV_NOPTS_VALUE)
                        throw std::runtime_error("first video frame has no timestamp; cannot align audio");
                    source_pts = last_source_pts + av_rescale_q(1, av_inv_q(input_rate), vin->time_base);
                    std::cerr << "[sync] interpolating missing video timestamp\n";
                }
                if (last_source_pts != AV_NOPTS_VALUE && source_pts <= last_source_pts)
                    throw std::runtime_error("source video timestamp moved backwards or repeated; restart after discontinuity");
                if (last_source_pts != AV_NOPTS_VALUE &&
                    av_rescale_q(source_pts - last_source_pts, vin->time_base, AV_TIME_BASE_Q) >
                    av_rescale_q(1, av_inv_q(input_rate), AV_TIME_BASE_Q) * 3 / 2) ++source_gaps;
                ++source_frames;
                frame->pts = last_source_pts = source_pts;
                {
                    if (frame->color_trc==AVCOL_TRC_SMPTE2084 || frame->color_trc==AVCOL_TRC_ARIB_STD_B67)
                        throw std::runtime_error("this YouTube preset is SDR; tone-map HDR input to SDR first");
                    AVRational sar = frame->sample_aspect_ratio;
                    if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1,1};
                    if (!video_filter.graph || frame->width != filter_width || frame->height != filter_height ||
                        frame->format != filter_format || av_cmp_q(sar, filter_sar) != 0) {
                        if (video_filter.graph) {
                            check(av_buffersrc_add_frame_flags(video_filter.source, nullptr, 0), "flush old conversion filters");
                            drain_video_filter();
                        }
                        std::ostringstream args;
                        args << "video_size=" << frame->width << "x" << frame->height
                             << ":pix_fmt=" << frame->format << ":time_base=" << vin->time_base.num << "/" << vin->time_base.den
                             << ":pixel_aspect=" << sar.num << "/" << sar.den
                             << ":frame_rate=" << input_rate.num << "/" << input_rate.den;
                        // Deinterlace when needed, preserve display aspect ratio,
                        // scale/pad, and convert to fixed 30 fps Rec.709 SDR.
                        video_filter.init(false, args.str(),
                            "bwdif=mode=send_field:parity=auto:deint=interlaced,"
                            "scale=w='if(gte(dar,16/9),1920,trunc(1080*dar/2)*2)':"
                            "h='if(gte(dar,16/9),trunc(1920/dar/2)*2,1080)':flags=bicubic:out_color_matrix=bt709:out_range=tv,"
                            "setsar=1,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,"
                            "fps=30,format=yuv420p,setparams=field_mode=prog:range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709,settb=1/90000");
                        filter_width = frame->width; filter_height = frame->height;
                        filter_format = frame->format; filter_sar = sar;
                        std::cerr << "[video] converting " << filter_width << "x" << filter_height << " -> 1920x1080p30\n";
                    }
                    check(av_buffersrc_add_frame_flags(video_filter.source, frame, AV_BUFFERSRC_FLAG_KEEP_REF), "convert video");
                    drain_video_filter();
                }
                av_frame_unref(frame);
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) check(ret, "decode video");
        };
        auto drain_audio_filter = [&]() {
            int ret;
            while ((ret = av_buffersink_get_frame(audio_filter.sink, afiltered)) >= 0) {
                afiltered->pts = av_rescale_q(afiltered->pts, av_buffersink_get_time_base(audio_filter.sink), aencCtx->time_base);
                check(avcodec_send_frame(aencCtx, afiltered), "encode AAC audio");
                drain_audio_encoder();
                av_frame_unref(afiltered);
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) check(ret, "receive converted audio");
        };
        auto drain_audio_decoder = [&]() {
            int ret;
            while ((ret = avcodec_receive_frame(adecCtx, aframe)) >= 0) {
                AVStream* ain = ifmt->streams[aIdx];
                int64_t pts = aframe->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) pts = aframe->pts;
                if (pts == AV_NOPTS_VALUE) pts = last_audio_pts;
                if (pts == AV_NOPTS_VALUE) throw std::runtime_error("first audio frame has no timestamp");
                aframe->pts = pts;
                last_audio_pts = pts + av_rescale_q(aframe->nb_samples, AVRational{1, aframe->sample_rate}, ain->time_base);
                if (!audio_filter.graph) {
                    char layout[128];
                    check(av_channel_layout_describe(&aframe->ch_layout, layout, sizeof(layout)), "describe audio layout");
                    std::ostringstream args;
                    args << "time_base=" << ain->time_base.num << "/" << ain->time_base.den
                         << ":sample_rate=" << aframe->sample_rate << ":sample_fmt=" << av_get_sample_fmt_name(static_cast<AVSampleFormat>(aframe->format))
                         << ":channel_layout=" << layout;
                    audio_filter.init(true, args.str(),
                        "aresample=44100:async=1000,aformat=sample_fmts=fltp:channel_layouts=stereo,asetnsamples=n=" + std::to_string(aencCtx->frame_size) + ":p=1");
                }
                check(av_buffersrc_add_frame_flags(audio_filter.source, aframe, AV_BUFFERSRC_FLAG_KEEP_REF), "convert audio");
                drain_audio_filter();
                av_frame_unref(aframe);
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) check(ret, "decode audio");
        };

        reader = std::make_unique<PacketReader>(ifmt, vIdx, aIdx);
        int ret = AVERROR_EOF;
        while (!interrupted && (ret = reader->next(pkt)) >= 0) {
            if (pkt->stream_index == vIdx) {
                check(avcodec_send_packet(decCtx, pkt), "send video packet");
                drain_decoder();
            } else if (pkt->stream_index==aIdx && adecCtx) {
                int64_t dts=pkt->dts==AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                if (dts==AV_NOPTS_VALUE && last_audio_pts==AV_NOPTS_VALUE) {
                    ++skipped_audio;
                    av_packet_unref(pkt);
                    continue;
                }
                if (dts!=AV_NOPTS_VALUE && last_audio_packet_dts!=AV_NOPTS_VALUE && dts<=last_audio_packet_dts) {
                    if (last_audio_packet_dts-dts>av_rescale_q(1,AVRational{1,1},ifmt->streams[aIdx]->time_base))
                        throw std::runtime_error("audio source clock reset; restart the encoder");
                    ++skipped_audio;
                    av_packet_unref(pkt);
                    continue;
                }
                if (dts!=AV_NOPTS_VALUE) last_audio_packet_dts=dts;
                check(avcodec_send_packet(adecCtx,pkt),"send audio packet");
                drain_audio_decoder();
            }
            av_packet_unref(pkt);
        }
        // A network disconnect may report EIO rather than EOF. Always drain
        // buffered media before reporting that input failure to the caller.
        int read_error = (!interrupted && ret != AVERROR_EOF) ? ret : 0;
        // Drain decoder first so B-frame/reordering delay cannot lose final video.
        check(avcodec_send_packet(decCtx, nullptr), "flush video decoder");
        drain_decoder();
        if (video_filter.graph) {
            check(av_buffersrc_add_frame_flags(video_filter.source, nullptr, 0), "flush video conversion");
            drain_video_filter();
        }
        if (adecCtx) {
            check(avcodec_send_packet(adecCtx, nullptr), "flush audio decoder");
            drain_audio_decoder();
            if (audio_filter.graph) {
                check(av_buffersrc_add_frame_flags(audio_filter.source, nullptr, 0), "flush audio conversion");
                drain_audio_filter();
            }
        }
        check(avcodec_send_frame(aencCtx,nullptr),"flush AAC encoder");
        drain_audio_encoder();
        check(avcodec_send_frame(encCtx, nullptr), "flush video encoder");
        drain_encoder();
        check(av_write_trailer(ofmt), "write output trailer");
        report_stats();
        check(read_error, "read input (output flushed)");
    } catch (const std::exception& error) {
        if (interrupted) {
            std::cerr << "Stopped.\n";
        } else {
            std::cerr << "Error: " << error.what() << "\n";
            status=1;
        }
    }

    const bool cancelled=interrupted;
    // Close the publisher before the reader destructor sets the interrupt flag.
    if (ofmt) {
        if (flv_pacer.transport) {
            int ret = flv_pacer.close();
            ofmt->pb = nullptr;
            if (ret < 0 && !cancelled) status = 1;
        } else if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb) {
            int ret = avio_closep(&ofmt->pb);
            if (ret < 0 && !cancelled) status = 1;
        }
        avformat_free_context(ofmt);
    }
    reader.reset(); // join before freeing the demuxer or its streams
    if (udp >= 0) close(udp);
    av_frame_free(&frame);
    av_frame_free(&filtered);
    av_frame_free(&aframe);
    av_frame_free(&afiltered);
    av_dict_free(&input_options);
    avcodec_free_context(&adecCtx);
    avcodec_free_context(&aencCtx);
    av_packet_free(&pkt);
    av_packet_free(&opkt);
    avcodec_free_context(&decCtx);
    avcodec_free_context(&encCtx);
    avformat_close_input(&ifmt);
    avformat_network_deinit();
    return status;
}
