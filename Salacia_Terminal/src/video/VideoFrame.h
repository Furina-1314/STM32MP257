#pragma once

#include <cstdint>
#include <vector>

namespace salacia {

// 跨线程视频帧（解码后拷贝至系统内存；紧排列，无行填充）
//  - 显示通道：BGRA（bytesPerPixel=4，QImage/OpenGL 直接可用）
//  - AI 通道：RGB（bytesPerPixel=3，已缩放至模型输入尺寸）
struct VideoFrame
{
    int width = 0;
    int height = 0;
    int bytesPerPixel = 4;
    std::int64_t timestampNs = 0; // GstBuffer PTS（可能为 NONE）
    std::vector<std::uint8_t> data;
};

} // namespace salacia
