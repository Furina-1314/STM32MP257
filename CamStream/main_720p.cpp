/*
 * RGB24(摄像头) --OpenCL(GPU)--> NV12 --appsrc--> GStreamer 硬件H264编码 --UDP--> RTP 推流
 * 模式: 720p @ 30fps (传感器仍按 1080p 节拍运行, postproc 硬件缩放至 720p)
 *
 * 链路实现见 core/ 共享库(Phase 1 重构, 原 920 行 main 已拆分为):
 *   dcmipp_setup / v4l2_capture / cl_converter / gst_streamer / stream_app
 * 本文件仅为 720p 默认配置薄壳.
 *
 * 用法: ./rgb_720p [采集设备] [目标IP] [端口] [码率kbps] [帧率] [命名参数...]
 * 默认: /dev/video1 192.168.1.100 5000 3000 0
 * 帧率参数=0(默认)不降帧: 传感器保持模式默认 30fps 节拍(与 720p 目标一致);
 * 帧率参数>0 时按板级校准模型节拍到指定帧率(vblank 于 STREAMON 前设置,
 * 流状态下写入不生效; vblank 跨运行持久, 启动时统一复位/设置)
 * 命名参数: --sensor-gain N(默认35)  --sensor-exposure N(默认2000)
 *           --isp-profile tl84|d50|none (默认 tl84, STREAMON 后自动应用)
 *           --isp-contrast none|50|200|dynamic   --require-isp
 *           --dump-frame N | --dump-every N --dump-count M
 *           --input-mode hostptr|copy   --gop N   --colorimetry bt601|bt709
 * Ctrl-C 退出并清理资源.
 */

#include "core/app_config.h"
#include "core/stream_app.h"

int main(int argc, char *argv[]) {
    camstream::AppConfig cfg =
        camstream::parseArgs(argc, argv, camstream::defaultConfig720p());
    return camstream::runStreamApp(cfg);
}
