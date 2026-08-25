#pragma once

#include <QString>

#include <vector>

#include "core/DataManager.h" // Detection 结果结构
#include "video/VideoFrame.h"

namespace salacia {

// 推理引擎纯虚基类（算法可替换性边界）
//
// 约定：
//  - 参数解耦红线：模型路径、输入尺寸、置信度/NMS 阈值均取自 AppConfig，
//    实现内禁止硬编码；
//  - 实现类必须以 Worker-Object 形态工作（moveToThread 独立推理线程，
//    禁止重写 QThread::run()）；
//  - initialize()/infer() 只应在推理工作线程内被调用；
//  - 输入帧为 RGB 紧排列（管线内 GPU 已缩放对齐至模型输入尺寸），
//    网络输出必须做边界校验后再使用。
class IModelInfer
{
public:
    virtual ~IModelInfer() = default;

    // 加载模型、创建会话、绑定执行后端（动态 EP 探测）；成功返回 true
    virtual bool initialize() = 0;

    // 单帧推理：输入已对齐帧，输出经阈值过滤与 NMS 的检测集合
    virtual std::vector<Detection> infer(const VideoFrame& frame) = 0;

    // 当前绑定的执行后端（如 "DirectML"/"CUDA"/"CPU"，供日志与 UI）
    virtual QString backendName() const = 0;

    // 模型输入尺寸（会话静态形状优先，动态形状回退配置值）
    virtual int inputWidth() const = 0;
    virtual int inputHeight() const = 0;
};

} // namespace salacia
