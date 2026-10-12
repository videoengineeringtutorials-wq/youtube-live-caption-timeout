#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
}

// Keep network reads ahead of paced encoding. In particular, downloading the
// next HLS segment must not block the frame clock. Queue size stays bounded.
class PacketReader {
    AVFormatContext* input;
    int video, audio;
    std::deque<AVPacket*> packets;
    size_t queued_bytes = 0;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool stopping = false, finished = false;
    int terminal_error = AVERROR_EOF;

    void run() {
        for (;;) {
            AVPacket* packet = av_packet_alloc();
            int result = packet ? av_read_frame(input, packet) : AVERROR(ENOMEM);
            std::unique_lock<std::mutex> lock(mutex);
            if (result < 0 || stopping || interrupted) {
                av_packet_free(&packet);
                terminal_error = result < 0 ? result : AVERROR_EXIT;
                finished = true;
                changed.notify_all();
                return;
            }
            if (packet->stream_index != video && packet->stream_index != audio) {
                av_packet_free(&packet);
                continue;
            }
            changed.wait(lock, [&]() {
                return stopping || interrupted ||
                    (packets.size() < 512 && (packets.empty() || queued_bytes + packet->size <= 8 * 1024 * 1024));
            });
            if (stopping || interrupted) {
                av_packet_free(&packet);
                finished = true;
                terminal_error = AVERROR_EXIT;
                changed.notify_all();
                return;
            }
            queued_bytes += packet->size;
            packets.push_back(packet);
            changed.notify_all();
        }
    }
public:
    PacketReader(AVFormatContext* context, int v, int a): input(context), video(v), audio(a) {
        worker = std::thread([this]() { run(); });
    }
    ~PacketReader() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            interrupted = 1; // release FFmpeg's interruptible network read
            changed.notify_all();
        }
        worker.join();
        for (AVPacket* packet : packets) av_packet_free(&packet);
    }
    int next(AVPacket* destination) {
        std::unique_lock<std::mutex> lock(mutex);
        while (packets.empty() && !finished && !interrupted)
            changed.wait_for(lock, std::chrono::milliseconds(50));
        if (interrupted) return AVERROR_EXIT;
        if (packets.empty()) return terminal_error;
        AVPacket* packet = packets.front();
        packets.pop_front();
        queued_bytes -= packet->size;
        av_packet_move_ref(destination, packet);
        av_packet_free(&packet);
        changed.notify_all();
        return 0;
    }
};
