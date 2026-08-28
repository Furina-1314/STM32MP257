#include "cl_converter.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>

namespace camstream {

// ====== OpenCL 内核源码 ======
static std::string readKernelFile(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open kernel file: " << filename << std::endl;
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

ClConverter::ClConverter()
    : platform_(nullptr), device_(nullptr), context_(nullptr),
      queue_(nullptr), program_(nullptr), kernel_(nullptr),
      input_mode_(INPUT_HOST_PTR), input_span_(0), input_copy_(nullptr),
      out_buffer_{nullptr, nullptr}, lws_(nullptr) {}

ClConverter::~ClConverter() {
    destroy();
}

void ClConverter::destroy() {
    // 镜像原 main cleanup 的释放顺序; 幂等, 可在析构前显式调用
    if (kernel_) { clReleaseKernel(kernel_); kernel_ = nullptr; }
    if (program_) { clReleaseProgram(program_); program_ = nullptr; }
    if (queue_) { clReleaseCommandQueue(queue_); queue_ = nullptr; }
    if (context_) { clReleaseContext(context_); context_ = nullptr; }
    for (size_t i = 0; i < input_clmem_.size(); i++)
        if (input_clmem_[i]) clReleaseMemObject(input_clmem_[i]);
    input_clmem_.clear();
    if (input_copy_) { clReleaseMemObject(input_copy_); input_copy_ = nullptr; }
    if (out_buffer_[0]) { clReleaseMemObject(out_buffer_[0]); out_buffer_[0] = nullptr; }
    if (out_buffer_[1]) { clReleaseMemObject(out_buffer_[1]); out_buffer_[1] = nullptr; }
    lws_ = nullptr;
}

cl_int ClConverter::init(uint32_t width, uint32_t height, uint32_t in_stride,
                         size_t input_span, size_t output_size,
                         const std::vector<CaptureBuffer> &cap_bufs,
                         const char *kernel_file, InputMode input_mode,
                         const char *build_options) {
    cl_int err = CL_SUCCESS;
    input_mode_ = input_mode;
    input_span_ = input_span;

    // 显式 local size: 部分驱动自动选择极差(与原实现一致)
    global_work_size_[0] = width / 2;
    global_work_size_[1] = height / 2;
    local_work_size_[0] = 16;
    local_work_size_[1] = 4;
    lws_ = (global_work_size_[0] % local_work_size_[0] == 0 &&
            global_work_size_[1] % local_work_size_[1] == 0)
               ? local_work_size_ : nullptr;

    // ------ OpenCL 初始化 ------
    err = clGetPlatformIDs(1, &platform_, nullptr);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clGetPlatformIDs]: " << err << std::endl;
        return err;
    }

    err = clGetDeviceIDs(platform_, CL_DEVICE_TYPE_GPU, 1, &device_, nullptr);
    if (err == CL_DEVICE_NOT_FOUND) {
        std::cout << "No GPU found, falling back to CPU." << std::endl;
        err = clGetDeviceIDs(platform_, CL_DEVICE_TYPE_CPU, 1, &device_, nullptr);
    }
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clGetDeviceIDs]: " << err << std::endl;
        return err;
    }

    // 打印实际执行内核的设备(诊断: 确认真的是 GPU 而非 CPU 回退实现)
    {
        char dev_name[128] = {0}, dev_vendor[128] = {0};
        clGetDeviceInfo(device_, CL_DEVICE_NAME, sizeof(dev_name) - 1, dev_name, nullptr);
        clGetDeviceInfo(device_, CL_DEVICE_VENDOR, sizeof(dev_vendor) - 1, dev_vendor, nullptr);
        std::cout << "OpenCL device: " << dev_name << " (" << dev_vendor << ")" << std::endl;
    }

    context_ = clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clCreateContext]: " << err << std::endl;
        return err;
    }

#ifdef CL_VERSION_2_0
    queue_ = clCreateCommandQueueWithProperties(context_, device_, nullptr, &err);
#else
    queue_ = clCreateCommandQueue(context_, device_, 0, &err);
#endif
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clCreateCommandQueue]: " << err << std::endl;
        return err;
    }

    kernel_source_ = readKernelFile(kernel_file);
    if (kernel_source_.empty()) {
        std::cerr << "Failed to read kernel file" << std::endl;
        return CL_INVALID_VALUE;   // 非 cl 错误, 任意非成功码, 由调用方终止
    }
    const char *src = kernel_source_.c_str();
    size_t src_len = kernel_source_.length();
    program_ = clCreateProgramWithSource(context_, 1, &src, &src_len, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clCreateProgramWithSource]: " << err << std::endl;
        return err;
    }

    err = clBuildProgram(program_, 1, &device_, build_options, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t log_size = 0;
        clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        std::vector<char> log(log_size + 1);
        clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
        std::cerr << "Kernel build error: " << log.data() << std::endl;
        return err;
    }
    kernel_ = clCreateKernel(program_, "rgb888_to_nv12", &err);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clCreateKernel]: " << err << std::endl;
        return err;
    }

    // 输入缓冲:
    //   HOST_PTR(默认): 对每个 V4L2 mmap 缓冲建 USE_HOST_PTR 零拷贝 cl_mem.
    //   V4L2 缓冲是非缓存 DMA 内存, GPU 直读一致(见文件头注释);
    //   内核访问跨度按 in_stride 计算, 覆盖行对齐填充的情况.
    //   COPY: 单个普通缓冲, 每帧 enqueueConvert 时显式整帧写入.
    if (input_mode_ == INPUT_COPY) {
        input_copy_ = clCreateBuffer(context_, CL_MEM_READ_ONLY,
                                     input_span, nullptr, &err);
        if (err != CL_SUCCESS) {
            std::cerr << "OpenCL Error [clCreateBuffer (input copy)]: "
                      << err << std::endl;
            return err;
        }
    } else {
        input_clmem_.resize(cap_bufs.size());
        for (size_t i = 0; i < cap_bufs.size(); i++) {
            input_clmem_[i] = clCreateBuffer(context_,
                                             CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                             input_span, cap_bufs[i].start, &err);
            if (err != CL_SUCCESS) {
                std::cerr << "OpenCL Error [clCreateBuffer (input USE_HOST_PTR)]: "
                          << err << std::endl;
                return err;
            }
        }
    }
    out_buffer_[0] = clCreateBuffer(context_, CL_MEM_WRITE_ONLY, output_size, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clCreateBuffer (output 0)]: " << err << std::endl;
        return err;
    }
    out_buffer_[1] = clCreateBuffer(context_, CL_MEM_WRITE_ONLY, output_size, nullptr, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clCreateBuffer (output 1)]: " << err << std::endl;
        return err;
    }

    // 固定内核参数; arg0(输入, 每帧切换到当前采集缓冲)与 arg1(输出, 双缓冲)
    // 在 enqueueConvert 中按帧设置. y_stride = width(NV12 紧凑布局).
    const cl_uint w = width, h = height, s = in_stride, y_s = width;
    err = clSetKernelArg(kernel_, 2, sizeof(cl_uint), &w);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clSetKernelArg (width)]: " << err << std::endl;
        return err;
    }
    err = clSetKernelArg(kernel_, 3, sizeof(cl_uint), &h);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clSetKernelArg (height)]: " << err << std::endl;
        return err;
    }
    err = clSetKernelArg(kernel_, 4, sizeof(cl_uint), &s);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clSetKernelArg (in_stride)]: " << err << std::endl;
        return err;
    }
    err = clSetKernelArg(kernel_, 5, sizeof(cl_uint), &y_s);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clSetKernelArg (y_stride)]: " << err << std::endl;
        return err;
    }
    return CL_SUCCESS;
}

cl_int ClConverter::enqueueConvert(const void *frame_host_ptr, size_t cap_index,
                                   int slot, cl_event *kernel_event) {
    cl_mem input = nullptr;
    if (input_mode_ == INPUT_COPY) {
        // 诊断模式: CL_TRUE 阻塞整帧写入, 彻底排除异步时序/一致性问题;
        // CPU 从非缓存 DMA 内存拷贝很慢(1080p 数十 ms 级), 仅用于 A/B 诊断
        cl_int err = clEnqueueWriteBuffer(queue_, input_copy_, CL_TRUE, 0,
                                          input_span_, frame_host_ptr,
                                          0, nullptr, nullptr);
        if (err != CL_SUCCESS) {
            std::cerr << "OpenCL Error [clEnqueueWriteBuffer (input copy)]: "
                      << err << std::endl;
            return err;
        }
        input = input_copy_;
    } else {
        input = input_clmem_[cap_index];
    }
    cl_int err = clSetKernelArg(kernel_, 0, sizeof(cl_mem), &input);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clSetKernelArg (input)]: " << err << std::endl;
        return err;
    }
    err = clSetKernelArg(kernel_, 1, sizeof(cl_mem), &out_buffer_[slot]);
    if (err != CL_SUCCESS) {
        std::cerr << "OpenCL Error [clSetKernelArg (output)]: " << err << std::endl;
        return err;
    }
    return clEnqueueNDRangeKernel(queue_, kernel_, 2, nullptr, global_work_size_,
                                  lws_, 0, nullptr, kernel_event);
}

cl_int ClConverter::enqueueReadback(int slot, void *dst, size_t size, cl_event *event) {
    return clEnqueueReadBuffer(queue_, out_buffer_[slot], CL_FALSE, 0,
                               size, dst, 0, nullptr, event);
}

} // namespace camstream
