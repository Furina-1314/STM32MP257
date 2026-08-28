/*
 * stream_app.h - 完整推流链路组装与主循环
 *
 * Phase 1 重构: 原 main 主体逐字搬运至此, 调用 camstream_core 各模块:
 *   setup_dcmipp_pipeline -> prepare_sensor_timing -> open_capture(STREAMON)
 *   -> probe_capture_fps -> ClConverter::init -> GstStreamer::build
 *   -> 主循环(dq -> GPU转换 -> 上帧推送 -> kx -> 还缓冲 -> 回读)
 * 两个 main(main_1080p/main_720p)只保留各自默认配置并调用 runStreamApp().
 */

#ifndef CAMSTREAM_STREAM_APP_H
#define CAMSTREAM_STREAM_APP_H

#include "app_config.h"

namespace camstream {

// 组装完整链路并运行主循环(Ctrl-C 退出). 返回进程退出码.
int runStreamApp(const AppConfig &cfg);

} // namespace camstream

#endif // CAMSTREAM_STREAM_APP_H
