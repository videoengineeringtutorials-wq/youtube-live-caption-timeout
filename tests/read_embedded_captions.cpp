// Independently decode EIA-608 side data from the encoded H.264 stream.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

int main(int argc, char** argv) {
    if (argc!=2 && (argc!=3 || (std::string(argv[2])!="--final" && std::string(argv[2])!="--timeline"))) return 1;
    const bool final_only=argc==3 && std::string(argv[2])=="--final";
    const bool timeline=argc==3 && std::string(argv[2])=="--timeline";
    std::string final_screen;
    AVFormatContext* input=nullptr;
    assert(avformat_open_input(&input,argv[1],nullptr,nullptr)>=0);
    assert(avformat_find_stream_info(input,nullptr)>=0);
    int idx=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    assert(idx>=0);
    const AVCodec* decoder=avcodec_find_decoder(input->streams[idx]->codecpar->codec_id);
    AVCodecContext* video=avcodec_alloc_context3(decoder);
    assert(avcodec_parameters_to_context(video,input->streams[idx]->codecpar)>=0);
    assert(avcodec_open2(video,decoder,nullptr)>=0);
    const AVCodec* ccdecoder=avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    AVCodecContext* captions=avcodec_alloc_context3(ccdecoder);
    captions->pkt_timebase=input->streams[idx]->time_base;
    assert(av_opt_set(captions->priv_data,"real_time","1",0)==0);
    assert(avcodec_open2(captions,ccdecoder,nullptr)==0);
    AVPacket* packet=av_packet_alloc();
    AVPacket* cc=av_packet_alloc();
    AVFrame* frame=av_frame_alloc();
    auto drain=[&]() {
        while (avcodec_receive_frame(video,frame)==0) {
            AVFrameSideData* data=av_frame_get_side_data(frame,AV_FRAME_DATA_A53_CC);
            if (data) {
                assert(av_new_packet(cc,data->size)==0);
                std::memcpy(cc->data,data->data,data->size);
                cc->pts=frame->pts;
                AVSubtitle sub{};
                int got=0;
                assert(avcodec_decode_subtitle2(captions,&sub,&got,cc)>=0);
                if (got) {
                    final_screen.clear();
                    for (unsigned i=0;i<sub.num_rects;++i)
                        if (sub.rects[i]->ass) {
                            final_screen += sub.rects[i]->ass;
                            if (!final_only && !timeline) std::cout << sub.rects[i]->ass << '\n';
                        }
                    if (timeline) std::cout << av_rescale_q(frame->best_effort_timestamp,
                        input->streams[idx]->time_base, AVRational{1,1000}) << '\t' << final_screen << '\n';
                }
                avsubtitle_free(&sub);
                av_packet_unref(cc);
            }
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(input,packet)>=0) {
        if (packet->stream_index==idx) {
            assert(avcodec_send_packet(video,packet)>=0);
            drain();
        }
        av_packet_unref(packet);
    }
    assert(avcodec_send_packet(video,nullptr)>=0);
    drain();
    if (final_only) std::cout << final_screen << '\n';
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_packet_free(&cc);
    avcodec_free_context(&video);
    avcodec_free_context(&captions);
    avformat_close_input(&input);
}
