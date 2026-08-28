/*
 * sensor_control.h - V4L2 subdev 控件的 ioctl 直控
 *
 * (plan §4.8/§4.9) 替代 system("v4l2-ctl -c ..."):
 *   - 按 v4l2-ctl 风格名称解析控件("analogue_gain" <-> "Analogue Gain"),
 *     经 VIDIOC_QUERY_EXT_CTRL + NEXT_CTRL 枚举, 归一化匹配(忽略大小写/
 *     下划线/空格), 缓存 id/范围/类型;
 *   - 写入: clamp 到 [min,max] -> VIDIOC_S_EXT_CTRLS -> 回读 G_EXT_CTRLS,
 *     返回驱动 actual 值与钳位标志;
 *   - 不经 shell, 可拿到 errno 与控件真实范围.
 * 适用于 sensor subdev(analogue_gain/exposure/vblank)与
 * ISP subdev(gamma_correction)等任何 V4L2 控件节点.
 */

#ifndef CAMSTREAM_SENSOR_CONTROL_H
#define CAMSTREAM_SENSOR_CONTROL_H

#include <string>
#include <map>
#include <cstdint>

namespace camstream {

class SensorControl {
public:
    SensorControl();
    ~SensorControl();                    // 关闭 fd

    bool openNode(const std::string &v4l2_device_node);
    bool isOpen() const { return fd_ >= 0; }
    void closeNode();

    // 名称是否存在(结果缓存)
    bool resolve(const std::string &v4l2_name);

    bool get(const std::string &v4l2_name, int64_t &value);

    // 写入并回读: requested 超范围时 clamp 并置 clamped=true.
    // 返回 true 时 actual 为驱动实际值; errmsg 非空表示有告警(如回读不一致)
    bool set(const std::string &v4l2_name, int64_t requested,
             int64_t &actual, bool &clamped, std::string &errmsg);

    bool range(const std::string &v4l2_name,
               int64_t &minv, int64_t &maxv, int64_t &defv);

    const std::string &lastError() const { return last_err_; }

private:
    SensorControl(const SensorControl &);
    SensorControl &operator=(const SensorControl &);

    struct CtrlInfo {
        uint32_t id;
        bool     is64;      // INTEGER64 用 value64, 否则 value
        int64_t  minv, maxv, defv;
    };

    bool lookup(const std::string &v4l2_name, CtrlInfo &out);
    bool extCtrl(uint32_t request, const CtrlInfo &ci, int64_t &value);

    int fd_;
    std::map<std::string, CtrlInfo> cache_;   // key: 归一化名称
    std::string last_err_;
};

} // namespace camstream

#endif // CAMSTREAM_SENSOR_CONTROL_H
