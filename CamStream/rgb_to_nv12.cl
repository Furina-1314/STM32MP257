/*
 * OpenCL Kernel: RGB888 -> NV12 (BT.601 limited range)
 * 运行在 GPU 上, 每个 work-item 转换一个 2x2 像素块:
 *   - 写出 4 个 Y 值 (vstore2 按行成对写入)
 *   - 4 个像素 RGB 取平均后转换出 1 对 U/V (NV12 4:2:0 采样)
 * 相比"每 work-item 一个像素"的写法, 线程数减少 4 倍, 消除了
 * x/y 奇偶分支, 且一半像素 UV 计算被浪费的问题不复存在;
 * 对 tile 架构的 GPU (Vivante) 内存访问局部性也更好.
 *
 * 布局约定:
 *   input : RGB24, 每像素 3 字节 (R,G,B), 行跨距 in_stride (V4L2 bytesperline)
 *   output: NV12, Y 平面 (height 行, 每行 y_stride 字节) 后紧跟
 *           交错 UV 平面 (height/2 行, 每行 y_stride 字节, U V U V ...)
 */

inline int rgb_to_y(int r, int g, int b)
{
    // Y = (66R + 129G + 25B + 128) >> 8 + 16
    return ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
}

inline int rgb_to_u(int r, int g, int b)
{
    // U(Cb) = (-38R - 74G + 112B + 128) >> 8 + 128
    return ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
}

inline int rgb_to_v(int r, int g, int b)
{
    // V(Cr) = (112R - 94G - 18B + 128) >> 8 + 128
    return ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

__kernel void rgb888_to_nv12(
    __global const uchar * restrict input,   // RGB24 输入
    __global uchar * restrict output,        // NV12 输出
    const uint width,                        // 图像宽度(像素, 偶数)
    const uint height,                       // 图像高度(像素, 偶数)
    const uint in_stride,                    // 输入行跨距(字节)
    const uint y_stride                      // 输出 Y/UV 平面行跨距(字节)
) {
    // 本 work-item 负责 2x2 块的左上角像素坐标
    const int bx = (int)(get_global_id(0) << 1);
    const int by = (int)(get_global_id(1) << 1);
    if (bx + 1 >= (int)width || by + 1 >= (int)height) return;

    // 一次读入 4 个像素 (vload3 允许非对齐访问)
    const int base = by * (int)in_stride + bx * 3;
    const int down = (int)in_stride;
    const int3 p00 = convert_int3(vload3(0, input + base));
    const int3 p10 = convert_int3(vload3(0, input + base + 3));
    const int3 p01 = convert_int3(vload3(0, input + base + down));
    const int3 p11 = convert_int3(vload3(0, input + base + down + 3));

    // Y 平面: 两行, 每行用 vstore2 写两个相邻像素
    const int yrow = by * (int)y_stride + bx;
    vstore2((uchar2)(convert_uchar_sat(rgb_to_y(p00.x, p00.y, p00.z)),
                     convert_uchar_sat(rgb_to_y(p10.x, p10.y, p10.z))),
            0, output + yrow);
    vstore2((uchar2)(convert_uchar_sat(rgb_to_y(p01.x, p01.y, p01.z)),
                     convert_uchar_sat(rgb_to_y(p11.x, p11.y, p11.z))),
            0, output + yrow + (int)y_stride);

    // UV 平面: 4 像素 RGB 取平均 -> 1 对 U/V, 交错写入
    const int3 avg = (p00 + p10 + p01 + p11) >> 2;
    __global uchar *uv = output + (int)y_stride * (int)height
                               + (by >> 1) * (int)y_stride + bx;
    uv[0] = convert_uchar_sat(rgb_to_u(avg.x, avg.y, avg.z));
    uv[1] = convert_uchar_sat(rgb_to_v(avg.x, avg.y, avg.z));
}
