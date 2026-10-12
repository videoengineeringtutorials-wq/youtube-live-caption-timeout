#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
}

// Use muxed FLV media DTS as the send clock. Both audio and video use the
// same anchor, including encoder delay and audio preceding the first picture.
struct FLVPacer {
    AVIOContext* bridge = nullptr;
    AVIOContext* transport = nullptr;
    std::vector<uint8_t> pending;
    bool header_sent = false, started = false;
    int64_t previous_ms = 0, elapsed_ms = 0, start_wall = 0, max_late_us = 0;

    int forward(const uint8_t* data, int size) {
        avio_write(transport, data, size);
        avio_flush(transport);
        return transport->error;
    }
    bool pace(uint32_t timestamp) {
        if (!started) {
            started = true;
            previous_ms = timestamp;
            start_wall = av_gettime_relative();
        } else {
            int64_t delta = int64_t{timestamp} - previous_ms;
            if (delta < -(int64_t{1}<<31)) delta += int64_t{1}<<32;
            if (delta > (int64_t{1}<<31)) delta -= int64_t{1}<<32;
            if (delta < 0) return false;
            elapsed_ms += delta;
            previous_ms = timestamp;
        }
        int64_t target = start_wall + elapsed_ms * 1000;
        while (!interrupted) {
            int64_t remaining = target - av_gettime_relative();
            if (remaining <= 0) break;
            av_usleep(std::min<int64_t>(remaining, 50000));
        }
        if (!interrupted) max_late_us = std::max(max_late_us, av_gettime_relative()-target);
        return true;
    }
    static uint32_t be32(const uint8_t* p) {
        return (uint32_t{p[0]}<<24)|(uint32_t{p[1]}<<16)|(uint32_t{p[2]}<<8)|p[3];
    }
    static int write(void* opaque, uint8_t* data, int size) {
        auto& self = *static_cast<FLVPacer*>(opaque);
        self.pending.insert(self.pending.end(), data, data+size);
        size_t used = 0;
        if (!self.header_sent) {
            if (self.pending.size()<9) return size;
            if (self.pending[0]!='F' || self.pending[1]!='L' || self.pending[2]!='V') return AVERROR_INVALIDDATA;
            uint32_t offset=be32(self.pending.data()+5);
            if (offset<9 || offset>65536) return AVERROR_INVALIDDATA;
            if (self.pending.size()<size_t(offset)+4) return size;
            int ret=self.forward(self.pending.data(),offset+4);
            if (ret<0) return ret;
            used=offset+4;
            self.header_sent=true;
        }
        while (self.pending.size()-used>=11) {
            const uint8_t* tag=self.pending.data()+used;
            size_t payload=(size_t{tag[1]}<<16)|(size_t{tag[2]}<<8)|tag[3];
            size_t total=11+payload+4;
            if (self.pending.size()-used<total) break;
            if (be32(tag+11+payload)!=11+payload) return AVERROR_INVALIDDATA;
            // Ignore metadata and AVC/AAC sequence headers for clock anchoring.
            bool media=payload>=2 && ((tag[0]==9 && (tag[11]&15)==7 && tag[12]==1) ||
                (tag[0]==8 && (tag[11]>>4)==10 && tag[12]==1));
            uint32_t timestamp=(uint32_t{tag[7]}<<24)|(uint32_t{tag[4]}<<16)|(uint32_t{tag[5]}<<8)|tag[6];
            if (media && !self.pace(timestamp)) return AVERROR_INVALIDDATA;
            int ret=self.forward(tag,total);
            if (ret<0) return ret;
            used+=total;
        }
        if (used) self.pending.erase(self.pending.begin(),self.pending.begin()+used);
        return size;
    }
    AVIOContext* attach(AVIOContext* destination) {
        transport=destination;
        auto* buffer=static_cast<uint8_t*>(av_malloc(32768));
        if (!buffer) throw std::bad_alloc();
        bridge=avio_alloc_context(buffer,32768,1,this,nullptr,write,nullptr);
        if (!bridge) { av_free(buffer); throw std::bad_alloc(); }
        return bridge;
    }
    int close() {
        int ret=0;
        if (bridge) {
            avio_flush(bridge);
            ret=bridge->error;
            if (!pending.empty() && !ret) ret=AVERROR_INVALIDDATA;
            av_freep(&bridge->buffer);
            avio_context_free(&bridge);
        }
        if (transport) {
            int closed=avio_closep(&transport);
            if (closed<0) ret=closed;
        }
        return ret;
    }
};
