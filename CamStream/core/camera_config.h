/*
 * camera_config.h - 运行时配置保存/加载(plan §15)
 *
 * 简单 INI 文本(key=value, '#'注释), 不依赖外部库:
 *   sensor_gain=35
 *   sensor_exposure=2000
 *   isp_profile=tl84          (tl84|d50|none)
 *   isp_contrast=none         (none|50|200|dynamic)
 *   input_mode=hostptr        (hostptr|copy)
 *   colorimetry=bt601         (bt601|bt709)
 *   bitrate_kbps=4000
 *   gop=0                     (0=默认公式)
 *   appsrc_queue=1
 *   fps=0                     (0=不降帧)
 *
 * 应用分两类(由控制台 load 命令处理):
 *   实时生效: sensor_gain/exposure, isp_profile/contrast(重跑 dcmipp-isp-ctrl)
 *   重启生效: bitrate/gop/colorimetry/input_mode/appsrc_queue/fps
 *             (编码器属性在管线构建时确定, 仅保存并提示)
 */

#ifndef CAMSTREAM_CAMERA_CONFIG_H
#define CAMSTREAM_CAMERA_CONFIG_H

#include <string>

#include "app_config.h"

namespace camstream {

class CameraConfig {
public:
    // 保存当前生效配置. 返回 false 表示文件写入失败.
    static bool save(const std::string &path, const AppConfig &cfg);

    // 读取 INI 到 cfg(在当前值基础上覆盖出现的键, 缺省键保持不变).
    // 未知键告警并忽略; 返回 false 表示文件打开/解析失败.
    static bool load(const std::string &path, AppConfig &cfg);
};

} // namespace camstream

#endif // CAMSTREAM_CAMERA_CONFIG_H
