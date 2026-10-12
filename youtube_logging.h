#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/log.h>
}

static std::vector<std::string> youtube_secrets;
static void youtube_log(void* context, int level, const char* format, va_list args) {
    if (interrupted || level>av_log_get_level()) return;
    char line[16384];
    static thread_local int prefix=1;
    int length=av_log_format_line2(context,level,format,args,line,sizeof(line),&prefix);
    if (length<0 || length>=static_cast<int>(sizeof(line))) {
        std::fputs("[FFmpeg] oversized log message omitted\n",stderr);
        return;
    }
    std::string text=line;
    for (const auto& secret:youtube_secrets) {
        if (secret.empty()) continue;
        size_t pos=0;
        while ((pos=text.find(secret,pos))!=std::string::npos) {
            text.replace(pos,secret.size(),"[redacted]");
            pos+=10;
        }
    }
    std::fputs(text.c_str(),stderr);
}
