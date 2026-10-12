#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
static std::atomic<bool> interrupted{false};
#include "../flv_pacer.h"

static std::vector<uint8_t> tag(uint32_t timestamp, bool media=true) {
    std::vector<uint8_t> t(21,0);
    t[0]=9; t[3]=6;
    t[4]=timestamp>>16; t[5]=timestamp>>8; t[6]=timestamp; t[7]=timestamp>>24;
    t[11]=0x17; t[12]=media ? 1 : 0; t.back()=17;
    return t;
}
int main() {
    FILE* file=std::tmpfile();assert(file);
    AVIOContext* transport=nullptr;
    std::string path="pipe:"+std::to_string(fileno(file));
    assert(avio_open(&transport,path.c_str(),AVIO_FLAG_WRITE)==0);
    FLVPacer pacer;
    auto* output=pacer.attach(transport);
    std::vector<uint8_t> data={'F','L','V',1,5,0,0,0,9,0,0,0,0};
    auto header=tag(0,false), first=tag(0xfffffff6), next=tag(10);
    for (const auto& t:{header,first,next}) data.insert(data.end(),t.begin(),t.end());
    int64_t began=av_gettime_relative();
    // Arbitrary AVIO fragmentation must preserve every byte and complete tag.
    for (uint8_t byte:data) { avio_w8(output,byte);avio_flush(output);assert(output->error==0); }
    assert(pacer.elapsed_ms==20); // media clock wraps after 2^32 milliseconds
    assert(av_gettime_relative()-began>=20000);
    auto backward=tag(9);
    avio_write(output,backward.data(),backward.size());avio_flush(output);
    assert(output->error==AVERROR_INVALIDDATA);
    assert(pacer.close()==AVERROR_INVALIDDATA);
    std::rewind(file);
    std::vector<uint8_t> result(data.size());
    assert(std::fread(result.data(),1,result.size(),file)==result.size());
    assert(result==data);
    std::fclose(file);
    std::puts("PASS: fragmented FLV, common A/V clock, timestamp wrap, byte preservation and backward-clock rejection");
}
