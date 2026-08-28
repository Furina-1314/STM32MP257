#include "sensor_control.h"

#include <cstring>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

namespace camstream {

// 名称归一化: 忽略大小写, 去掉 '_'/' '/'-',
// 使 "analogue_gain" 与驱动注册名 "Analogue Gain" 等价匹配
static std::string normalizeName(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '_' || c == ' ' || c == '-') continue;
        out += (char)tolower((unsigned char)c);
    }
    return out;
}

SensorControl::SensorControl() : fd_(-1) {}

SensorControl::~SensorControl() { closeNode(); }

bool SensorControl::openNode(const std::string &node) {
    closeNode();
    fd_ = ::open(node.c_str(), O_RDWR);
    if (fd_ < 0)
        last_err_ = "open " + node + ": " + strerror(errno);
    return fd_ >= 0;
}

void SensorControl::closeNode() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SensorControl::resolve(const std::string &v4l2_name) {
    CtrlInfo ci;
    if (lookup(v4l2_name, ci))
        return true;
    last_err_ = "control '" + v4l2_name + "' not found on device";
    return false;
}

// NEXT_CTRL 枚举全部控件, 归一化匹配; 命中与沿途枚举到的控件都进缓存
bool SensorControl::lookup(const std::string &v4l2_name, CtrlInfo &out) {
    if (fd_ < 0) {
        last_err_ = "device not open";
        return false;
    }
    const std::string want = normalizeName(v4l2_name);

    std::map<std::string, CtrlInfo>::const_iterator it = cache_.find(want);
    if (it != cache_.end()) {
        out = it->second;
        return true;
    }

    uint32_t id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (true) {
        struct v4l2_query_ext_ctrl qc;
        memset(&qc, 0, sizeof(qc));
        qc.id = id;
        if (ioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &qc) != 0)
            break;   // 枚举结束(或内核不支持 NEXT_CTRL)
        id = qc.id | V4L2_CTRL_FLAG_NEXT_CTRL;
        if (qc.flags & V4L2_CTRL_FLAG_DISABLED)
            continue;

        const std::string got = normalizeName(qc.name);
        if (cache_.find(got) == cache_.end()) {
            CtrlInfo ci;
            ci.id = qc.id;
            ci.is64 = (qc.type == V4L2_CTRL_TYPE_INTEGER64);
            ci.minv = qc.minimum;
            ci.maxv = qc.maximum;
            ci.defv = qc.default_value;
            cache_[got] = ci;
        }
        if (got == want) {
            out = cache_[got];
            return true;
        }
    }
    return false;
}

// 单控件 EXT_CTRLS 读/写, 控件 class 由 id 推导
bool SensorControl::extCtrl(uint32_t request, const CtrlInfo &ci, int64_t &value) {
    struct v4l2_ext_control ec;
    struct v4l2_ext_controls ecs;
    memset(&ec, 0, sizeof(ec));
    ec.id = ci.id;
    if (ci.is64)
        ec.value64 = value;
    else
        ec.value = (int32_t)value;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_ID2CLASS(ci.id);
    ecs.count = 1;
    ecs.controls = &ec;
    if (ioctl(fd_, request, &ecs) != 0) {
        last_err_ = std::string("v4l2 ext ctrl ioctl: ") + strerror(errno);
        return false;
    }
    value = ci.is64 ? ec.value64 : (int64_t)ec.value;
    return true;
}

bool SensorControl::get(const std::string &v4l2_name, int64_t &value) {
    CtrlInfo ci;
    if (!lookup(v4l2_name, ci))
        return false;
    return extCtrl(VIDIOC_G_EXT_CTRLS, ci, value);
}

bool SensorControl::set(const std::string &v4l2_name, int64_t requested,
                        int64_t &actual, bool &clamped, std::string &errmsg) {
    actual = requested;
    clamped = false;
    errmsg.clear();
    CtrlInfo ci;
    if (!lookup(v4l2_name, ci)) {
        errmsg = "control '" + v4l2_name + "' not found";
        return false;
    }
    int64_t v = requested;
    if (v < ci.minv) { v = ci.minv; clamped = true; }
    if (v > ci.maxv) { v = ci.maxv; clamped = true; }
    if (!extCtrl(VIDIOC_S_EXT_CTRLS, ci, v)) {
        errmsg = last_err_;
        return false;
    }
    // 回读: 驱动可能再次修正请求值(如模式切换后重置), actual 以回读为准
    int64_t rb = v;
    if (!extCtrl(VIDIOC_G_EXT_CTRLS, ci, rb)) {
        errmsg = last_err_;
        return false;
    }
    actual = rb;
    if (rb != v)
        errmsg = "readback != requested value";
    return true;
}

bool SensorControl::range(const std::string &v4l2_name,
                          int64_t &minv, int64_t &maxv, int64_t &defv) {
    CtrlInfo ci;
    if (!lookup(v4l2_name, ci))
        return false;
    minv = ci.minv;
    maxv = ci.maxv;
    defv = ci.defv;
    return true;
}

} // namespace camstream
