#pragma once

#include <stdexcept>
#include <string>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/error.h>
}

static inline void media_check(int result, const char* operation)
{
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}

struct MediaFilter {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    MediaFilter() = default;
    MediaFilter(const MediaFilter&) = delete;
    MediaFilter& operator=(const MediaFilter&) = delete;
    ~MediaFilter() { reset(); }
    void reset() {
        avfilter_graph_free(&graph);
        source = sink = nullptr;
    }
    void init(bool audio, const std::string& args, const std::string& filters) {
        reset();
        graph = avfilter_graph_alloc();
        if (!graph) throw std::bad_alloc();
        graph->nb_threads = 4;
        media_check(avfilter_graph_create_filter(&source,
            avfilter_get_by_name(audio ? "abuffer" : "buffer"), "input", args.c_str(), nullptr, graph), "create filter input");
        media_check(avfilter_graph_create_filter(&sink,
            avfilter_get_by_name(audio ? "abuffersink" : "buffersink"), "output", nullptr, nullptr, graph), "create filter output");
        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            throw std::bad_alloc();
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source;
        outputs->pad_idx = 0;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        inputs->pad_idx = 0;
        int ret = avfilter_graph_parse_ptr(graph, filters.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        media_check(ret, "parse conversion filters");
        media_check(avfilter_graph_config(graph, nullptr), "configure conversion filters");
    }
};
