#include "frame_dumper.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>

namespace camstream {

FrameDumper::FrameDumper()
    : mode_(0), frame_no_(0), every_(0), count_(0),
      requested_(0), dumped_(0) {}

void FrameDumper::configureFrame(uint64_t frame_no) {
    mode_ = 1;
    frame_no_ = frame_no;
}

void FrameDumper::configureEvery(uint64_t every, uint64_t count) {
    mode_ = 2;
    every_ = every;
    count_ = count;
}

void FrameDumper::requestNext(uint64_t n) {
    if (n > 0)
        requested_.fetch_add(n);
}

bool FrameDumper::shouldDump(uint64_t frame_no) {
    if (mode_ == 1) {
        if (frame_no == frame_no_)
            return true;
    } else if (mode_ == 2) {
        if (every_ > 0 && (count_ == 0 || dumped_ < count_) &&
            (frame_no % every_) == 0)
            return true;
    }
    // 运行时请求: 消耗一个连续帧名额
    uint64_t req = requested_.load();
    while (req > 0) {
        if (requested_.compare_exchange_weak(req, req - 1))
            return true;   // CAS 失败时 req 已被刷新为当前值, 重试
    }
    return false;
}

bool FrameDumper::dump(uint64_t frame_no, const uint8_t *nv12, size_t size,
                       std::string &path_out) {
    path_out.clear();
    if (::mkdir("debug", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[WARN] mkdir debug failed: %s\n", strerror(errno));
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "debug/frame_%06llu.nv12",
             (unsigned long long)frame_no);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[WARN] open %s failed: %s\n", path, strerror(errno));
        return false;
    }
    size_t w = fwrite(nv12, 1, size, f);
    fclose(f);
    if (w != size) {
        fprintf(stderr, "[WARN] short write %s\n", path);
        return false;
    }
    dumped_++;
    path_out = path;
    return true;
}

} // namespace camstream
