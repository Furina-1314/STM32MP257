/*
 * isp_controller.h - DCMIPP ISP profile 应用控制器
 *
 * (plan §7) 在 camera pipeline STREAMON 成功后自动执行 ST 官方工具
 * /usr/local/demo/bin/dcmipp-isp-ctrl, 把 IMX335 的 ISP 调色参数
 * (白平衡增益/CCM 等, TL84/D50 profile) 应用到 DCMIPP ISP.
 *
 * 已实机验证: 手工执行 `dcmipp-isp-ctrl -i 1 -v` 可恢复全局偏色画面,
 * 本控制器把该操作产品化为启动流程的一部分.
 *
 * 执行方式: posix_spawn + argv 数组(不拼 shell 字符串, plan §4.9),
 * 子进程 stdout/stderr 经管道捕获后由调用方在状态区启动前打印(plan §6.4).
 * 运行期切换(illuminant/contrast/auto-gain)供 Phase 4 控制台复用.
 */

#ifndef CAMSTREAM_ISP_CONTROLLER_H
#define CAMSTREAM_ISP_CONTROLLER_H

#include <string>
#include <vector>

namespace camstream {

// (plan §7.3) ISP 启动配置
struct IspStartupConfig {
    bool        enabled = true;      // 默认自动应用
    int         illuminant = 1;      // 0=D50, 1=TL84(默认, 已实机验证)
    int         contrast = -1;       // -1=不设置, 0=none 1=50% 2=200% 3=dynamic
    bool        verbose = true;      // 附加 -v
    bool        required = false;    // --require-isp: 失败则启动失败(默认仅 WARN)
    std::string tool_path = "/usr/local/demo/bin/dcmipp-isp-ctrl";
};

struct IspApplyResult {
    bool        ran = false;         // 是否真的执行了(工具缺失/spawn失败为 false)
    int         exit_code = -1;      // 子进程退出码(ran=true 时有效)
    std::string output;              // 子进程 stdout+stderr 捕获
    std::string cmdline;             // 实际执行的命令行(日志用)
};

class IspController {
public:
    IspController();
    explicit IspController(const IspStartupConfig &cfg);

    // 应用当前配置(profile + 可选 contrast). 必须在 camera pipeline
    // STREAMON 成功之后调用(ST 要求 pipeline 正在运行).
    IspApplyResult apply() const;

    // ---- 运行期切换(重跑 dcmipp-isp-ctrl, 供 Phase 4 控制台) ----
    IspApplyResult setIlluminant(int type);   // 0=D50, 1=TL84
    IspApplyResult setContrast(int type);     // 0=none 1=50% 2=200% 3=dynamic
    IspApplyResult runAutoGain();             // -g: 传感器增益/曝光自动调整

    const IspStartupConfig &config() const { return cfg_; }
    void setConfig(const IspStartupConfig &cfg) { cfg_ = cfg; }

    // 名称映射(日志用)
    static const char *illuminantName(int t);   // "D50"/"TL84"
    static const char *contrastName(int t);     // "none"/"50%"/"200%"/"dynamic"

private:
    // 执行 tool_path + args(argv 数组, 管道捕获输出, 阻塞至退出)
    IspApplyResult run(const std::vector<std::string> &args) const;

    IspStartupConfig cfg_;
};

} // namespace camstream

#endif // CAMSTREAM_ISP_CONTROLLER_H
