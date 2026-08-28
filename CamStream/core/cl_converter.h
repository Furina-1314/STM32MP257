/*
 * cl_converter.h - OpenCL RGB888 -> NV12 转换器
 *
 * 从原 main 逐字搬运(Phase 1 行为不变), 资源生命周期封装为类:
 *   - init()            : platform/device(GPU, CPU 回退)/context/queue/
 *                         kernel 编译/每采集缓冲一个 USE_HOST_PTR 输入
 *                         cl_mem(零拷贝)/双缓冲输出 cl_mem/固定内核参数
 *   - enqueueConvert()  : 绑定采集缓冲与输出槽位并提交内核(原主循环热路径)
 *   - enqueueReadback() : 非阻塞回读输出缓冲(event 通知, 双缓冲流水线)
 *   - ~ClConverter()    : 镜像原 cleanup 的释放顺序
 *
 * USE_HOST_PTR 零拷贝依据(原文件头注释): V4L2 缓冲是非缓存 DMA 内存,
 * GPU DMA 读取天然一致; 内核访问跨度按 in_stride 计算(input_span).
 */

#ifndef CAMSTREAM_CL_CONVERTER_H
#define CAMSTREAM_CL_CONVERTER_H

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

// OpenCL headers
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 300
#endif
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include "v4l2_capture.h"   // CaptureBuffer

namespace camstream {

// 输入模式(plan §4.2):
//   INPUT_HOST_PTR: 每个采集缓冲一个 USE_HOST_PTR cl_mem(零拷贝, 默认, 性能基准)
//   INPUT_COPY    : 单个普通输入缓冲, 每帧 clEnqueueWriteBuffer(CL_TRUE) 整帧
//                   显式拷贝 —— 彻底排除 USE_HOST_PTR 的异步一致性问题,
//                   代价是 CPU 从非缓存 DMA 内存拷贝很慢(1080p 明显降帧),
//                   仅作为局部块异常时的 A/B 诊断手段
enum InputMode {
    INPUT_HOST_PTR = 0,
    INPUT_COPY = 1
};

class ClConverter {
public:
    ClConverter();
    ~ClConverter();   // 释放全部已创建资源(半初始化状态亦可安全析构)

    // 初始化全部 OpenCL 资源. 返回 CL_SUCCESS 或首个错误码(此时资源半建,
    // 由析构清理). kernel_file 与原实现一致为运行目录下的 rgb_to_nv12.cl;
    // build_options 传给 clBuildProgram(如 "-DUSE_BT709", 默认 ""=BT.601).
    cl_int init(uint32_t width, uint32_t height, uint32_t in_stride,
                size_t input_span, size_t output_size,
                const std::vector<CaptureBuffer> &cap_bufs,
                const char *kernel_file, InputMode input_mode,
                const char *build_options = "");

    // 每帧: 绑定第 cap_index 个采集缓冲(输入)与第 slot(0/1) 个输出缓冲,
    // 提交 2D NDRange 内核(全局 {W/2,H/2}, 局部 {16,4} 与原实现相同).
    // frame_host_ptr 为该帧 V4L2 mmap 地址, 仅 INPUT_COPY 模式使用.
    cl_int enqueueConvert(const void *frame_host_ptr, size_t cap_index,
                          int slot, cl_event *kernel_event);

    // 非阻塞回读第 slot 个输出缓冲(CL_FALSE + event), 供双缓冲流水线.
    cl_int enqueueReadback(int slot, void *dst, size_t size, cl_event *event);

    bool ready() const { return kernel_ != nullptr; }

    // 立即释放全部 OpenCL 资源(幂等, 析构函数再调一次无副作用).
    // USE_HOST_PTR 输入 cl_mem 对 V4L2 mmap 缓冲持有 GPU 侧映射, 必须在
    // close_capture 释放 CMA 页之前 destroy, 否则内核报
    // "N pages are still in use"(每缓冲一次, CMA 泄漏).
    void destroy();

private:
    ClConverter(const ClConverter &);            // 持有 cl 资源, 禁拷贝
    ClConverter &operator=(const ClConverter &);

    cl_platform_id platform_;
    cl_device_id device_;
    cl_context context_;
    cl_command_queue queue_;
    cl_program program_;
    cl_kernel kernel_;
    InputMode input_mode_;
    size_t input_span_;
    std::vector<cl_mem> input_clmem_;   // 每个采集缓冲一个, USE_HOST_PTR 零拷贝
    cl_mem input_copy_;                 // INPUT_COPY 模式的普通输入缓冲
    cl_mem out_buffer_[2];              // 输出双缓冲
    size_t global_work_size_[2];
    size_t local_work_size_[2];
    const size_t *lws_;
    std::string kernel_source_;
};

} // namespace camstream

#endif // CAMSTREAM_CL_CONVERTER_H
