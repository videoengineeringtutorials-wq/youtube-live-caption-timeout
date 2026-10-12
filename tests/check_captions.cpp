// Decode generated CC1 with FFmpeg's independent EIA-608 decoder.
#include "../captions_608.h"
#include <cassert>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

struct Receiver {
    AVCodecContext* ctx;
    int64_t ticks = 0;
    std::string screen;
    Receiver() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
        assert(codec);
        ctx = avcodec_alloc_context3(codec);
        ctx->pkt_timebase = AVRational{1, 1000000};
        assert(av_opt_set(ctx->priv_data, "real_time", "1", 0) == 0);
        assert(avcodec_open2(ctx, codec, nullptr) == 0);
    }
    ~Receiver() { avcodec_free_context(&ctx); }
    void frame(CCEncoder& enc) {
        std::vector<uint8_t> data;
        build_cc(data, enc);
        assert(data.size() == 6 && data[0] == 0xFC && data[3] == 0xF9);
        for (size_t i : {size_t(1), size_t(2), size_t(4), size_t(5)})
            assert(__builtin_popcount(data[i]) % 2 == 1);
        AVPacket* packet = av_packet_alloc();
        assert(av_new_packet(packet, data.size()) == 0);
        std::copy(data.begin(), data.end(), packet->data);
        packet->pts = ticks++ * 33367;
        AVSubtitle sub{};
        int got = 0;
        assert(avcodec_decode_subtitle2(ctx, &sub, &got, packet) >= 0);
        if (got) {
            screen.clear();
            for (unsigned i = 0; i < sub.num_rects; ++i)
                if (sub.rects[i]->ass) screen += sub.rects[i]->ass;
        }
        avsubtitle_free(&sub);
        av_packet_free(&packet);
    }
    void drain(CCEncoder& enc) {
        while (!enc.pending.empty()) frame(enc);
        for (int i = 0; i < 12; ++i) frame(enc);
    }
};

int main() {
    for (CaptionMode mode : {CaptionMode::RollUp, CaptionMode::PopOn}) {
        CCEncoder enc{};
        enc.mode = mode;
        Receiver rx;
        set_caption(enc, "FIRST LINE");
        rx.drain(enc);
        set_caption(enc, "SECOND LINE");
        rx.drain(enc);
        std::cout << rx.screen << '\n';
        assert(rx.screen.find("FIRST LINE") != std::string::npos);
        assert(rx.screen.find("SECOND LINE") != std::string::npos);
        // FFmpeg's 384x288 caption canvas maps rows 14/15 to y=228/243.
        assert(rx.screen.find("\\pos(38,228)") != std::string::npos);
        assert(rx.screen.find("\\pos(38,243)") != std::string::npos);
        set_caption(enc, "SECOND LINE"); // repeated UDP text must not roll again
        assert(enc.pending.empty());
        set_caption(enc, "THIRD LINE");
        rx.drain(enc);
        assert(rx.screen.find("FIRST LINE") == std::string::npos);
        assert(rx.screen.find("SECOND LINE") != std::string::npos);
        assert(rx.screen.find("THIRD LINE") != std::string::npos);
        set_caption(enc, "QUEUED FIRST");
        set_caption(enc, "QUEUED SECOND"); // must finish both, even before emission
        rx.drain(enc);
        assert(rx.screen.find("QUEUED FIRST") != std::string::npos);
        assert(rx.screen.find("QUEUED SECOND") != std::string::npos);
        set_caption(enc, "TOP EXPLICIT\nBOTTOM EXPLICIT");
        rx.drain(enc);
        assert(rx.screen.find("TOP EXPLICIT") != std::string::npos);
        assert(rx.screen.find("BOTTOM EXPLICIT") != std::string::npos);
        assert(rx.screen.find("THIRD LINE") == std::string::npos);
        set_caption(enc, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456\nZYXWVUTSRQPONMLKJIHGFEDCBA654321");
        rx.drain(enc);
        set_caption(enc, "SHORT TOP\nSHORT BOTTOM");
        rx.drain(enc);
        assert(rx.screen.find("SHORT TOP") != std::string::npos);
        assert(rx.screen.find("SHORT BOTTOM") != std::string::npos);
        assert(rx.screen.find("\\pos(38,228)") != std::string::npos);
        assert(rx.screen.find("\\pos(38,243)") != std::string::npos);
        assert(rx.screen.find("KLMNOP") == std::string::npos);
        assert(rx.screen.find("654321") == std::string::npos);
        set_caption(enc, "NEXT");
        rx.drain(enc);
        assert(rx.screen.find("SHORT BOTTOM") != std::string::npos);
        assert(rx.screen.find("NEXT") != std::string::npos);
        assert(rx.screen.find("SHORT TOP") == std::string::npos);
        set_caption(enc, "\n");
        rx.drain(enc);
        assert(rx.screen.find("NEXT") == std::string::npos);
        assert(rx.screen.find("SHORT BOTTOM") == std::string::npos);
        set_caption(enc, "AFTER CLEAR");
        rx.drain(enc);
        assert(rx.screen.find("AFTER CLEAR") != std::string::npos);
        assert(rx.screen.find("NEXT") == std::string::npos);
    }
    auto wrapped = caption_lines("This sentence is longer than thirty two characters and wraps");
    assert(wrapped.size() == 2 && wrapped[0].size() <= 32 && wrapped[1].size() <= 32);
    assert(caption_lines("hello\r\nworld\n").size() == 2);
    std::cout << "PASS: bottom two rows, roll-up, pop-on, duplicates, wrapping, shorter replacements and blank clearing\n";
}
