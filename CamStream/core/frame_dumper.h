/*
 * frame_dumper.h - 按需 NV12 帧落盘(plan §4.3)
 *
 * 三种触发方式:
 *   1) 启动参数 --dump-frame N          : 只落盘第 N 帧(帧号与状态栏一致, 1 起)
 *   2) 启动参数 --dump-every N --dump-count M : 每 N 帧一帧, 共 M 帧(0=不限)
 *   3) 运行时命令 dump frame / dump next N    : 之后连续 N 帧逐帧落盘
 *
 * 输出: debug/frame_XXXXXX.nv12 (OpenCL 输出的完整 NV12, 位于编码之前,
 * 是"前端转换 vs 后端编码/网络"的分界诊断点).
 * 运行时请求经 atomic 计数, 控制台线程与主循环并发安全.
 */

#ifndef CAMSTREAM_FRAME_DUMPER_H
#define CAMSTREAM_FRAME_DUMPER_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <string>

namespace camstream {

class FrameDumper {
public:
    FrameDumper();

    void configureFrame(uint64_t frame_no);                // --dump-frame
    void configureEvery(uint64_t every, uint64_t count);   // --dump-every/--dump-count
    void requestNext(uint64_t n);                          // 运行时命令(线程安全)

    // 主循环每帧调用(frame_no 与状态栏一致, 1 起; 线程安全)
    bool shouldDump(uint64_t frame_no);

    // 落盘到 debug/frame_XXXXXX.nv12; 成功时 path_out 返回文件路径(供日志)
    bool dump(uint64_t frame_no, const uint8_t *nv12, size_t size,
              std::string &path_out);

    uint64_t dumped() const { return dumped_; }

private:
    // 0=off 1=single-frame 2=every
    int mode_;
    uint64_t frame_no_;
    uint64_t every_;
    uint64_t count_;                 // every 模式上限(0=不限)
    std::atomic<uint64_t> requested_; // 运行时请求的连续帧数
    uint64_t dumped_;
};

} // namespace camstream

#endif // CAMSTREAM_FRAME_DUMPER_H
