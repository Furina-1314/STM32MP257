# vibeplan.md — Salacia_Terminal 优化开发总计划

> 版本：v1.0（Phase 0 产出）
> 日期：2026-08-29
> 依据：《Salacia_Terminal_优化开发提示词》+《ROV_A35_M33_Control_Protocol_v1.0.md》（仅业务语义参考）+ 三路只读审计
> 执行纪律：一次一个 Phase，每 Phase 后停止等待"编译通过/继续"；不擅自 git commit/推送/PR；源码 GBK、ini/QML/Markdown UTF-8。

---

## 1. 现状架构（Phase 0 审计结论）

### 1.1 模块清单（src/ 现状，全部保留并增量优化）

| 模块 | 文件 | 职责 | 本任务动作 |
|---|---|---|---|
| 入口 | `main.cpp` | 崩溃转储、OpenGL RHI 前置、配置/日志初始化、TD-8 退出加固 | 微调（主题初始化） |
| 主窗口 | `MainWindow.h/.cpp` | 中央视频 + 左右坞 + 状态栏；装配四数据面 | **重构**为 Fluent 导航框架 |
| 配置 | `core/AppConfig.h/.cpp` | ini 单例（load-once），边界校验，路径落地解析 | **扩展**（新节键+校验+缺省禁用降级） |
| 数据中心 | `core/DataManager.h/.cpp` | shared_mutex 读写锁 + atomic 链路标志 + 无载荷信号 | **扩展**（TCP 传感器快照/告警/模式状态） |
| 日志 | `core/Logger.h/.cpp` | 后台线程异步日志，10MB 轮转 | 保留 |
| 环形缓冲 | `utils/RingBuffer.h` | 无锁 SPSC | 保留 |
| 视频管线 | `video/GStreamerPipeline.*`、`VideoFrame.h` | RTP/H264 接收、自愈、帧环 | **不动**（仅统计口径微调） |
| 视频渲染 | `widgets/VideoGLWidget.*` | QOpenGLWidget + 信箱 + 检测框/标签 | 平移进新主页布局 |
| 控制面板 | `widgets/ControlPanelWidget.*` | 16 路 PWM 滑条 + 紧急停机 | **重构**为 TCP 控制 ViewModel |
| 推理 | `recognition/IModelInfer.h`、`OnnxInferEngine.*` | ORT 动态 EP + YOLO 后处理 | 保留 |
| 遥测 | `communication/TelemetryPacket.h`、`UdpReceiver.*` | UDP:5001 v2 50 字节 + Mahony | **保留为兼容回退**（ini 开关） |
| SSH 客户端 | `communication/SshClient.*` | libssh Worker + FIFO 命令 | **Phase 5 删除** |
| 姿态解算 | `sensor/MPU6500Processor.*` | Mahony 六轴 | 保留（数据源切换） |
| 姿态模型 | `sensor/RovVizModel.*` + `qml/RovViz.qml` | Quick3D 适配 | 平移 |

### 1.2 线程与数据流（现状）

```
主线程(GUI)          GStreamer流线程×N      遥测Worker        SSH Worker(删)     AI Worker
MainWindow ──────► appsink回调 ─┐         UdpReceiver       SshClient         OnnxInferEngine
 VideoGLWidget33ms◄─RingBuffer(4)          │Mahony            │pwm CLI          │DML
 QQuickWidget(3D)◄─RovVizModel◄────────────┘                  │                  │
 状态栏5Hz◄────────DataManager(shared_mutex/atomic)◄──────────┴──────────────────┘
```
退出（TD-8 红线）：releaseGl → 管线 stopForExit(READY+故意泄漏) → 遥测停 → SSH停(删) → AI停 → Logger 停 → Sleep(500)+ExitProcess。

### 1.3 现状要点
- 无任何 TCP 代码（grep QTcpSocket/registry 零命中）；命令通道 = MainWindow:47 一处 lambda 硬拼 `"pwm <id> <us>"`。
- 无测试框架（无 QtTest/CTest/任何 *_test.cpp）。
- 工作区干净：仅 2 个未跟踪需求文档（本计划+协议文档，保留不删）。
- git：ZCode 分支（PR #2 开放中）；本任务期间不自动 commit。

---

## 2. 目标 / 非目标 / 范围边界

**目标**（全部仅 Windows 侧）：
1. 新增 Windows↔A35 TCP 客户端：连接/分帧/编码/解码/重连/状态同步；
2. 通信函数注册表（≥26 条目）替代散落命令拼装；
3. 100Hz 传感器汇总接收与显示（温湿度/MPU6500/电压SOC/DYP-RD），有效性/新鲜度区分；
4. FluentUIStyle 界面重构（浅色/Fluent/FluentUI3/frameless，导航页式布局，告警中心）；
5. 控制交互升级（10 舵机+6 推进器、horizontal 基准滑条、estop/emergency 独立通道）；
6. 全部运行时硬编码迁入 `config/app_config.ini`（唯一配置源）；
7. 删除 SSH 运行时模块；
8. QtTest 单元测试 + mock A35 server 集成测试。

**非目标**（禁止）：不改 A35/M33/板端任何代码；不实现 RPMsg/固件/硬件驱动；不改 UDP 视频行为；不提供远程 shell；不把 A35↔M33 ASCII 协议当 Windows↔A35 wire format；不建第二配置源；不跨 Phase 施工。

---

## 3. 接口清单

### 3.1 已确认（现状在用）
| 接口 | 状态 |
|---|---|
| UDP:5000 RTP/H264 视频（CamStream） | ✅ 在用，不动 |
| UDP:5001 遥测 v2（50B，CRC16-CCITT-FALSE） | ✅ 在用，降级为兼容回退（ini `network/telemetry_udp_enable`，默认开） |
| `config/app_config.ini`（即项目现有 config.ini） | ✅ 唯一配置源，继续扩展 |

### 3.2 缺失——Windows↔A35 TCP（候选草案，待 A35 团队确认后方可对接实机）
**帧格式候选**（小端、逐字段序列化、禁裸 struct 镜像）：

```
偏移  字段     宽度  说明
0     magic    u32   0x53414C41 ("ALAS")
4     version  u8    协议版本 1
5     funcId   u16   函数 ID（见 §4 注册表）
7     seq      u16   Windows 生成递增 0-65535 回绕；响应原样回带
9     flags    u8    bit0=需ACK bit1=事件帧(板端主动) bit2=错误响应
10    len      u16   payload 字节数（≤ max_payload）
12    payload  len   逐字段小端序列化
12+len crc16    u16   CRC16-CCITT-FALSE，覆盖偏移 0..11+len
```
帧头 12 字节；TCP 流式分帧靠 len；`max_payload`（默认 4096）超限即断线重连。

**100Hz 传感器汇总帧 payload 草案**（funcId=0x0100，A35→Win，30 字节）：
```
tempC f32 | humidPct f32 | ax f32 ay f32 az f32 gx f32 gy f32 gz f32
| voltage f32 | distMm f32 | validMask u8(bit0温湿 bit1 MPU bit2电压 bit3 DYP)
| boardTimeMs u32(低32位)
```
DYP-RD wire 单位暂定 mm；无效/超量程以 validMask+哨兵值（-1.0f）表达，**不用 0 冒充无效**。

### 3.3 待 A35 确认项（阻塞实机联调，不阻塞 mock 开发）
| # | 事项 | 影响 |
|---|---|---|
| P-01 | 帧格式/魔数/版本/CRC 全字段 | codec 对接 |
| P-02 | estop 专用 funcId 与 ACK 行为 | 紧急停机通道 |
| P-03 | 100Hz 帧字段/有效位/哨兵值定义 | 传感器显示 |
| P-04 | DYP-RD wire 单位/量程/无效值 | 距离显示与告警 |
| P-05 | 电池化学体系与 SOC 分段曲线参数 | SOC 标定（未确认前显示"待标定"） |
| P-06 | 心跳机制（周期帧 or TCP keepalive） | 在线判定 |
| P-07 | 错误码表（对齐协议文档 err bad_cmd 等） | 错误映射 |
| P-08 | A35 TCP 服务端口分配 | ini `[tcp] port` |
| P-09 | ACK/迟到响应/超时语义细节 | 状态机 |
| P-10 | `servo/propeller set` 值域确认（0-180° / ±100%） | 控件值域 |

---

## 4. 函数注册表设计（`src/communication/FunctionRegistry.h/.cpp`）

单例表，构建期静态注册；表项结构：

```cpp
struct FunctionEntry {
    quint16 funcId;            // 见下表
    const char* name;          // "set servo"
    Direction dir;             // Request / Response / Event / Both
    Category cat;              // System / Safety / Mode / Servo / Propeller / Sensor / Telemetry
    EncodeFn encode;           // 强类型参数 → payload（nullptr=空载荷）
    DecodeFn decode;           // payload → 强类型结果（校验长度/枚举/值域/NaN）
    bool needsAck; int ackTimeoutMs; Priority prio;   // prio: Estop > Emergency > Normal
    const char* vmUpdate;      // 关联 ViewModel 状态更新类型
    QMap<int,AlertLevel> errMap;  // 错误码 → 告警级别
    const char* degrade;       // 对端能力缺失时的 Windows 降级行为描述
};
```

| funcId | 名称 | 方向 | 编码要点 |
|---|---|---|---|
| 0x0001 | ask | Win→A35 | 空 |
| 0x0002 | ver | Win→A35 | 空；响应含版本串 |
| 0x0003 | status | Win→A35 | 空 |
| 0x0004 | help | Win→A35 | 空 |
| 0x0010 | stop | Win→A35 | 空（正常停止） |
| 0x0011 | emergency | Win→A35 | 空（紧急上浮，独立 funcId/状态/文案） |
| 0x0012 | **estop** | Win→A35 | **单帧**：10×u16 舵机零值 + 6×i16 推进器零值（ID 起点与单位按 P-10） |
| 0x0020/1 | safe on/off | Win→A35 | 空 |
| 0x0022/3 | horizontal on/off | Win→A35 | 空 |
| 0x0030 | set servo <id> <angle> | Win→A35 | u8 id + u16 angle(0-180)，逐台独立报文 |
| 0x0031 | set servo all <angle> | Win→A35 | u16 angle |
| 0x0032 | set servo mid | Win→A35 | u8 id / 0=all |
| 0x0033/4 | get servo id/all | Win→A35 | 查询；响应 u16[] |
| 0x0040 | set propeller <id> <value> | Win→A35 | u8 id + i16 value(-100..100)，逐台独立报文 |
| 0x0041 | set propeller all <value> | Win→A35 | i16 value |
| 0x0042 | set propeller stop | Win→A35 | u8 id / 0=all |
| 0x0043/4 | get propeller base/real | Win→A35 | id；响应 i16[] |
| 0x0050 | **统一基准值** | Win→A35 | 单帧 6×i16 同值（horizontal on 专用，与 estop 同为仅有的两个多设备单帧） |
| 0x0060/1/2 | sensor mpu/dyp/all | Win→A35 | 查询；响应文本/结构 |
| 0x00F0 | heartbeat | Win↔A35 | u32 时间戳（P-06 未确认前可选） |
| 0x0100 | 传感器汇总 100Hz | A35→Win | §3.2 payload 草案 |
| 0x0101 | ACK | A35→Win | 回带 seq + u16 errCode |
| 0x0102 | 状态事件 | A35→Win | 模式变化（safe/horizontal/estop/emergency 权威状态） |
| 0x0103 | 告警事件 | A35→Win | 级别+来源+文本（对齐 event safe_stop 等） |

接收流程：字节流缓存 → 提帧(magic/ver/len/crc) → 查表 funcId → decode 强类型 → 投递 Model/ViewModel（QueuedConnection）。发送流程：UI 意图 → 权限函数 → 查表 → encode → 优先级发送队列（estop/emergency 插队）→ ACK/超时更新。未知 funcId：计数+告警+丢弃不断线。

---

## 5. 配置审计与迁移方案

### 5.1 现有键（全部保留兼容）
`[network] host_ip/board_ip/rtp_port/telemetry_port`、`[log] dir/level`、`[video] jitter_latency_ms/preferred_decoder`、`[ai] enable/model_path/label_file/input_*/confidence_threshold/nms_iou_threshold/execution_provider`、`[rov] battery_full_voltage/battery_empty_voltage + ssh_*(Phase 5 删)`、`[control] servo_*/thruster_*`、`[imu] mahony_*`。

### 5.2 拟新增键（默认值一律写入 ini，不藏代码）

```ini
[tcp]
enable = true                ; TCP 控制通道总开关
host =                       ; 留空=用 [network] board_ip
port = 7000                  ; 待 P-08 确认
connect_timeout_ms = 3000
request_timeout_ms = 1000    ; ACK 超时
heartbeat_enable = true      ; 待 P-06
heartbeat_interval_ms = 1000
reconnect_enable = true
reconnect_base_ms = 1000     ; 指数退避基数
reconnect_max_ms = 10000     ; 退避上限
max_retry = 0                ; 0=无限
tcp_nodelay = true
recv_buffer_limit = 65536    ; 字节流缓存上限（超限断线重连）
max_payload = 4096
send_queue_capacity = 64     ; 溢出丢最旧（estop/emergency 永不丢弃）
sensor_expected_hz = 100
sensor_stale_ms = 500        ; 数据过期阈值

[network]
telemetry_udp_enable = true  ; UDP 遥测兼容回退开关（本轮决策）

[control]
servo_count = 10
thruster_count = 6
id_base = 1
servo_min_deg = 0
servo_max_deg = 180
servo_step_deg = 1
thruster_min_pct = -100
thruster_max_pct = 100
thruster_step_pct = 1
slider_rate_limit_ms = 50    ; 拖动发送限频
release_flush = true         ; 松开滑条立即发最终值

[battery]
cell_count = 4               ; 固定 4S（提示词 §9.3）
chemistry =                  ; 空=未标定 → 显示"待标定"
soc_curve =                  ; "v1,v2,...:s1,s2,..." 分段线性（空=待标定）
soc_filter_alpha = 0.2
soc_hysteresis_pct = 3
low_threshold_pct = 30
critical_threshold_pct = 15

[dyp]
unit = mm                    ; 显示单位（wire 单位待 P-04）
precision = 0
valid_min_mm = 20
valid_max_mm = 8000
stale_ms = 500
warn_distance_mm = 1000
danger_distance_mm = 300

[alarms]
max_items = 200
merge_window_ms = 5000
log_alarms = true

[ui]
theme = light                ; light/dark（默认浅色）
palette = fluent             ; fluent/teams
style = FluentUI3
text_refresh_hz = 5          ; GUI 文本刷新频率
attitude_render_hz = 20      ; 3D 姿态渲染频率（保持现状值）
angle_precision = 1
voltage_precision = 2
distance_precision = 0
estop_confirm = false        ; 紧急停机不弹确认
emergency_confirm = true     ; 紧急上浮二次确认
```

### 5.3 硬编码迁移表（Phase 0 审计，60+ 条，Phase 1 落地）

**IP/端口**：AppConfig.h:94-95/110/114、SshClient.h:39、GStreamerPipeline.h:85 默认值与 .cpp:281 提示文案 → 保持 `[network]`（新增 tcp 节见 5.2）。
**超时/等待**：SshClient.cpp:18-22/62-68（随 SSH 删除）；UdpReceiver.cpp:17-18 watchdog 500/1000ms → `[tcp] sensor_stale_ms` 复用 + `[network]` 遥测判活；各 Worker stop 阶梯 3000/2000/1000 → 统一 `[system]`（新增 `worker_stop_ladder_ms = 3000,2000,1000`，仅 Logger/AI/遥测/TCP 共用读）；main.cpp:76 Sleep(500) → `[system] exit_grace_ms`。
**定时器/频率**：VideoGLWidget:76 33ms → `[ui] video_render_interval_ms=33`；ControlPanel:18 50ms → `[control] slider_rate_limit_ms`；OnnxInferEngine:237 5ms → `[ai] poll_interval_ms`；GStreamer watchdog 500ms/2s 判活/重建延迟/统计 1Hz/无包提示 5s → `[video]` 新键；MainWindow 节流 200ms×2 → `[ui] text_refresh_hz`；状态栏消息时长 3000/5000/10000 → `[ui] status_message_ms` 档位。
**数量/范围**：ControlPanel kServoChannels=10/kThrusterChannels=6/setRange(0,180)/(-100,100)/kServoDefaultDeg=90/映射分母 180/200 → `[control]` 新键（数组尺寸改动态 QVector）。
**阈值/算法**：Mahony dt 限幅与增益上限 → `[imu]`；电池 clamp/线性 → `[battery]`（SOC 曲线化）；亮度权重等绘制常量保留（纯视觉，入 `[ui]` 可选项）。
**路径**：crash dump 模板、日志名模板 → `[log]`；data.yaml 绝对路径保持（用户实配）。
**精度**：MainWindow 'f',1/'f',0/'f',2 → `[ui] *_precision`。
**UI 尺寸/颜色**：窗口 1440×860 → `[ui] window_width/height`；清屏色/最小高/固定宽/按钮高/红色 → `[ui]`；RovViz.qml 颜色尺寸 → QML 常量文件 `src/qml/Theme.qml`（GBK 纪律不适用 QML，UTF-8）。
**管线字符串常量**（buffer-size 2MB/queue 2/appsink drop 等）→ `[video]` 可选键（默认现值，标注"仅高级调优"）。

### 5.4 校验与降级
- AppConfig 新增 `validate()`：缺关键键（tcp/port 等）→ Logger error + 状态栏 Error + 禁用 TCP 功能（UI 置灰），提示键名与文件路径；非关键显示项允许 ini 内默认值。
- 所有 bounded* 读取沿用（越界告警回退）；新增交叉约束：`reconnect_max >= base`、`soc 曲线单调递增`、`warn > danger`（dyp）等。

---

## 6. GUI 方案（FluentUIStyle，按用户修正）

### 6.1 库事实（审计实证）
- 纯 **C++ Qt Widgets** 样式库（QProxyStyle "FluentUI3" + ExWidgets 扩展控件 DLL），版本 0.1，MIT；非 QML。
- 接入：`app.setStyle("FluentUI3")`（插件或直链）+ `app.setProperty("_q_themestyle", 0)`（0=Fluent 配色）；浅/深主题 `FluentUIAppearance` 单例。
- Qt≥6.10 需 CorePrivate/GuiPrivate/WidgetsPrivate；MSVC 编 `/utf-8 /wd4273`；链 `dwmapi`。
- **frameless**：`ExWidgets/frameless`（FluentTitleBar/FluentWindowFrame）+ QWindowKit 1.5.0（3rd/qwindowkit，Apache-2.0）。仅 Windows，无跨平台裁剪。

### 6.2 复制清单（→ `src/ui/`，不引用外部路径）
1. `fluentui3style/`：fluentui3style.cpp/.h、fluentui3style_global.h、fluentui3styleproperties.h、fluentuiappearance.cpp/.h、palettemanager.cpp/.h、qstyleanimation.cpp+p.h、qstylehelper.cpp+p.h、qhexstring_p.h、comboboxpopupanimation/menupopupanimation _p.cpp/.h、resource.qrc+resource/（Segoe Fluent Icons.ttf+2png）——共 20 文件；
2. `FluentUI3Colors/fluentui3colors.h`（保持相对 include 或加 include 路径）；
3. ExWidgets 公共：exwidgetsmacros.h、exwidgets_global.h；控件子集：exwinuinavigationview（+exnavtreewidget+exstackedwidget）、excontentdialog、exmessagebox、exinfobar+exinfobarhost、exexpander、extabwidget；
4. `frameless/` 全部 + `3rd/qwindowkit` 全部。
- 复制源码含中文注释（UTF-8）→ CMake `set_source_files_properties(<ui 文件> PROPERTIES COMPILE_OPTIONS "/source-charset:utf-8")`（与全局 GBK 并存）；`EXWIDGETS_LIBRARY` 宏改为静态内编。

### 6.3 窗口结构
- FluentWindowFrame（frameless）+ FluentTitleBar；`app.setStyle("FluentUI3")`；默认**浅色 + Fluent 配色**（ini `[ui] theme/palette`，设置页可即时切换）。
- 左侧 `ExWinUINavigationView`：**主菜单 = 主页、指令；页脚区（下方）= 设置、关于**；默认展开可收起（仅图标+Tooltip）；当前页选中态。
- 主页：中上视频（VideoGLWidget 平移）、右上 Quick3D 姿态、右下传感器卡（温湿度/MPU 六轴+姿态/电压+SOC/DYP-RD，各带有效性与最后更新时间）、中下控制区（10 舵机竖滑条 + 6 推进器竖滑条或 horizontal 基准滑条 + 固定区红色"紧急停机"与独立"紧急上浮"）；顶部告警摘要条（可展开滚动列表，三级筛选/合并/条数上限）；底部统计栏（沿用视频/AI/遥测/链路四组 + 分别显示"TCP 已连接/协议可用/遥测正常/视频正常"）。
- 分辨率/DPI 适配：模块间 QSplitter 可拉动调宽、不可拖动改布局；紧急按钮最小尺寸保底；视频保持宽高比。
- 指令页：参数化表单覆盖注册表全部函数（安全/模式/系统/舵机/推进器/传感器查询），显示 Sequence/ACK/耗时/错误；高级原始入口仅限注册表已知函数、不得 shell。
- 设置页：主题/配色即时切换；关键运行参数摘要（network/tcp/video/ai）；"打开配置目录"与"修改后重启生效"提示。
- 关于页：软件名/版本/作者/协议版本（占位，待提供项不编造）。

### 6.4 Model/ViewModel/Service 分层
- Service：`TcpClient`（Worker-Object）、`FunctionRegistry`、`ConfigService`（=AppConfig 扩展）；
- Model：`SensorModel`（DataManager 扩展：TCP/UDP 双源新鲜度取优）、`AlarmModel`、`SafetyStateModel`（safe/horizontal/estop/emergency/连接/请求中/故障，集中权限函数）、`ControlViewModel`（目标值/已发送值/A35 确认值三态）；
- UI（Widgets + FluentUIStyle）只绑 ViewModel，不解析报文不碰 socket。

---

## 7. 权限矩阵（集中函数 `SafetyStateModel::canControl(...)`）

| 状态 | 舵机逐路 | 推进器逐路 | 基准滑条 | 紧急停机 | 紧急上浮 |
|---|---:|---:|---:|---:|---:|
| 普通手动 | 可用 | 可用 | 隐藏 | 可用 | 可用 |
| horizontal on 且非 safe | 置灰 | 隐藏/置灰 | 可用 | 可用 | 可用 |
| safe on | 置灰 | 置灰 | 置灰 | 可用 | 可用 |
| estop 已确认 | 置灰 | 置灰 | 置灰 | 显示已触发 | 按对端状态 |
| emergency 已确认 | 置灰 | 置灰 | 置灰 | 可用 | 显示进行中 |
| 断线/状态未知 | 置灰 | 置灰 | 置灰 | 无法下发 | 无法下发 |

模式以 A35 ACK/状态事件为准：发送→"请求中"→成功切换/失败恢复权威值或"未知"；被拒命令回滚最近确认值并提示原因。紧急反馈持续显示不被 Info 覆盖；解除仅调用已确认对端函数。

---

## 8. 风险 / 假设 / 回滚

| 风险 | 缓解 |
|---|---|
| FluentUIStyle 在 Qt 6.11 + MSVC 私有头兼容 | Phase 4 首个 commit 即最小样例验证（空窗口+样式），失败则按库 README 兼容表回退 fusion 基座 |
| 复制源码 UTF-8 与全局 GBK 冲突 | per-source `/source-charset:utf-8`（§6.2） |
| TD-8 退出路径回归 | closeEvent 序列不变，每 Phase 退出冒烟（退出码 0x0） |
| 编辑器把 GBK 存成 UTF-8+BOM | 已发生过一次；每 Phase 构建前脚本校验 GBK 完整性 |
| 100Hz+UI 压力 | RingBuffer/节流沿用；Phase 6 压测 |
| wire 草案与 A35 实现不符 | 全部 codec 常量集中 `WireConstants.h`，一处替换；mock 测试先构造后对齐 |
| 删除 SSH 引发控制真空 | 删除放在 Phase 5 末（TCP 控制+指令页已可用） |

回滚：每 Commit 保持可编译，git 分支 ZCode 上按 Commit 粒度回退；不自动 commit（用户指令后进行）。

---

## 9. 测试方案

- **框架**：QtTest（Qt 自带，无新依赖）；CMake 新增 `tests/` 子目录 + 独立测试可执行文件（不混入主程序），`enable_testing()` + ctest。
- **单元**（Phase 1-2）：codec 分帧（完整/半包/粘包/逐字节/多帧/非法长度/坏版本/未知 funcId/坏 CRC/截断载荷）、注册表完整性、AppConfig 新键校验/缺键降级、SOC 曲线/滤波/滞回、DYP 有效范围/告警分级。
- **mock A35 server**（Phase 2，`tests/mock_a35/`）：QTcpServer 实现——可配置行为（正常 ACK/错误码/迟到响应/不发 ACK/主动事件/告警/100Hz 汇总流/随即断连），脚本化用例。
- **集成**（Phase 2/6）：重连退避与不重放危险指令、estop 单帧 10+6 零值断言、100Hz 持续下 UI 不阻塞且普通/紧急发送及时、Sequence 匹配。
- **GUI/人工**（Phase 4-5）：导航/页面/权限矩阵置灰、滑条三态值、DPI 缩放无遮挡、主题切换、告警三级筛选。
- **回归**：UDP 视频/姿态/AI 标注真机行为；退出码 0x0。
- 命令：`cmake --build out/build/debug --target salacia_tests && ctest --test-dir out/build/debug --output-on-failure`（与主程序同 vcvars+Ninja 流程）。

---

## 10. Phase / Commit 计划

> 每 Phase 结束：运行该 Phase 编译+测试 → 按 §十五 报告 → **停止等待确认**。
> 通用编译：`vcvars64 && cmake --build out/build/debug`（Release 同）；通用测试：ctest 命令见 §9。

### Phase 0（本轮）——审计与 vibeplan.md
- C0.1 本文档。验收：文档完整覆盖 §4.2 全部条目。

### Phase 1 —— 配置集中化 + codec/函数表 + 单元测试 ✅ 已完成（2026-08-30）
- [x] C1.1 AppConfig 扩展：[tcp]/[battery]/[dyp]/[alarms]/[ui]/[system]/[control]/[network]/[video]/[ai]/[log] 新节键全部落地 + validate() 交叉约束 + tcpUsable() 缺关键键禁用降级 + logSummary 输出校验结果；全部超时/频率/数量/范围/精度硬编码迁移（ControlPanelWidget 动态通道数、MainWindow 节流/精度/消息时长、VideoGLWidget 节拍/线宽/字号、GStreamerPipeline 全部节拍与缓冲、OnnxInferEngine 轮询、UdpReceiver 判活、Logger 轮转阈值、Worker 停止阶梯、main 退出静默期）。验收：单测 10 项过、主程序冒烟行为不变。
- [x] C1.2 `WireConstants.h`（帧格式/funcId 枚举/优先级）+ `WireCodec`（小端字段编解码、流式分帧、CRC 复用 telemetryCrc16、坏帧重同步、resyncError 诊断字段、超长/溢出处置）+ `FunctionRegistry`（31 条目元数据表 + 强类型编解码：servo/propeller/estop 单帧 10+6 零值/base value/heartbeat/sensor summary/ack/列表）。验收：单测 14+12 项过。
- [x] C1.3 tests/ 框架（QtTest 三独立测试目标 + ctest；GBK/字符集与主目标同红线；窄化 QtTest 包含绕开 Qt 6.11.1 qrunnable.h 预览包缺陷）。验收：36/36 全绿。
- [x] C1.4 UI 常量迁移收尾：窗口尺寸/姿态区背景与最小高/检测框样式/紧急按钮配色/链路提示色/标签宽度入 [ui]；RovViz.qml 颜色材质灯光拆入 Theme.qml（qrc 同步）。验收：Debug/Release 双构建零告警。
- 环境备注：Qt 6.11.1 msvc2022_64 的 qrunnable.h:26 为 const 限定默认构造（预览包缺陷），经 `<QtTest>`→QtCore 伞头引入、仅在 /permissive- 下报 C7731——测试采用窄化包含规避；若后续 Qt 更新修复可恢复伞头。

### Phase 2 —— TCP 客户端 + 传感器模型 + mock ✅ 已完成（2026-08-30）
- [x] C2.1 `TcpClient` Worker：专属线程 socket（NoProxy 直连 + 可配 NODELAY）、双优先级发送队列（紧急通道永不丢、普通溢出丢最旧、5ms 冲刷）、指数退避重连（base*2^n 封顶，可配最大次数）、连接超时看门狗、seq 出队分配 + 挂起 ACK 表 + 超时扫描、迟到响应计数丢弃、超长/溢出判定失步断线、未知 funcId 计数告警不断线、断开清队列 + 首连后断线期间拒绝指令入队（不重放红线）、重连后自动 ask/status/sensor all 只读权威查询、心跳可配。强类型信号（sensorSummaryReady/ackReceived/eventReceived）。验收：14 集成用例全绿（含 100Hz 流、断连重连、estop 单帧 10+6 零值、连接期入队优先级、分段 ACK 重组装）。
- [x] C2.2 mock A35（`tests/mock_a35.*`）：可配自动 ACK/错误码/延迟/分段写回、100Hz 汇总流、计数断连、原始字节注入。`salacia_tests_tcp` 14 用例全绿。
- [x] C2.3 `SensorModel`：TCP/UDP 双源新鲜度取优（同新鲜窗口取更新者、双源过期全字段失效不用 0 冒充）、DYP-RD 六态分级（NotReady/Normal/Warning/Danger/OutOfRange/Stale）、SOC 空格分隔曲线分段线性 + 钳位 0-100 + 一阶滤波 + 滞回 + 低/严重低电量告警信号（曲线未标定 -> socCalibrated=false 显示"待标定"保留原始电压）、TCP 六轴独立 Mahony -> attitudeReady。10 用例全绿。
  - 设计调整：传感器快照以 SensorModel 为权威（current() 拉取 + displayUpdated 通知），未在 DataManager 增设平行存储，避免双写；姿态经 attitudeReady 桥接 DataManager::setRovState。
  - 环境发现：QSettings IniFormat 将含逗号的值解析为 QStringList（toString 得空串）——soc_curve 读取侧已做列表 join 兼容，推荐空格分隔写法。
- [x] C2.4 MainWindow 接线：tcpUsable() 门控创建、TCP 状态栏标签（在线/离线/禁用）、sensorSummaryReady->SensorModel、attitudeReady->DataManager、UDP RovState->SensorModel 双源、batteryAlarm->日志；closeEvent 逆序 视频->遥测->TCP->SSH->AI。
- 回归：Debug/Release 双构建零告警；60/60 测试全绿（10+14+12+10+14）；主程序冒烟四数据面齐动、退出干净、无代理告警（NoProxy 修复）。

### Phase 3 —— 告警中心 + 安全状态机 + 控制 ViewModel ✅ 已完成（2026-08-30）
- [x] C3.1 `AlarmModel`（src/core）：三级 Info/Warning/Error、同 source+summary 窗口内合并计数（保留首条时间/详情 + 刷新最近时间）、容量淘汰（先丢最旧低级别，Error 优先保留）、顶部摘要=最高级别中最近一条（Error 不被 Info 覆盖）、级别位掩码筛选、关联 seq、可选落日志（首条不打风暴）；可注入测试时钟。8 用例全绿。
- [x] C3.2 `SafetyStateModel`（src/core）：连接/权威已知双前提、safe/horizontal 三态（Unknown/Pending/On/Off，Pending 保守禁用）、estop/emergency 权威布尔；权限矩阵集中函数（canServoIndividual/canThrusterIndividual/canBaseSlider/baseSliderVisible/estopButton/emergencyButton，逐行覆盖提示词 §十 六行矩阵，含 horizontal on 时舵机也置灰、estop 已确认显示"已触发"、emergency 进行中、断线全锁）；请求生命周期（requestSent 只置 Pending 不当成功；ACK errCode!=0 回滚权威值并发 modeRejected；超时恢复权威值）；StateEvent 权威事件优先清除 Pending；断线清挂起回 Unknown。14 用例全绿。
- [x] C3.3 `ControlViewModel`（src/control）：10+6 通道三态（target/sent/confirmed，confirmedValid 区分"未知"与 0）、限频合并（slider_rate_limit_ms 每通道最新值）+ 松手立即冲刷（release_flush）、值域钳位与 id 越界拒绝、权限门控全部发送入口（blocked 发 permissionBlocked）、horizontal 切换清空逐路待发（隐藏控件旧值不发红线）+ 基准单帧 BaseValue（6 路同值、确认回填全部推进器）、estop/emergency 一键直发绕过节拍（estop 32 字节全零单帧；确认开关来自 [ui]）、ACK 确认/被拒回滚最近确认值、超时 channelUnknown 不自动重发。13 用例全绿。
- 配套：WireConstants 新增 StateEvent 位掩码（kStateSafe/Horizontal/Estop/Emergency）+ FunctionRegistry::decodeStateEvent（未知位整帧拒绝）。
- UI 接线按计划留待 Phase 4/5（FluentUI 重构时绑定）；模型层全部可独立测试。
- 回归：Debug/Release 双构建零告警；全量 95/95 用例绿（8 套件：10+14+12+10+14+8+14+13）；GBK 0 损坏；主程序冒烟正常。

### Phase 4 —— FluentUIStyle 接入 + 主界面重构 ✅ 已完成（2026-08-30）
- [x] C4.1 库接入：fluentui3style（17 文件+资源）/FluentUI3Colors/ExWidgets 子集（10 类）/frameless（FluentTitleBar+FluentWindowFrame）/qwindowkit 1.5.0 整树（examples 剔除）复制进 src/ui/，许可证随附（MIT/Apache-2.0）。静态库目标 salacia_fluentstyle/salacia_exwidgets/salacia_frameless + add_subdirectory(qwindowkit STATIC)。**关键工程决策**：全库源码统一转 GBK（与主工程同字符集；中文字面量改 QString::fromLocal8Bit 适配 Qt6 UTF-8 语义；qwindowkit 的 3 个 UTF-8 文件同转并剥离其 /utf-8 注入）——规避"UTF-8 中文注释在 GBK 翻译单元吞行尾"的解析破坏（已实验证实）与 D8016 字符集冲突；Qt>=6.10 私有头（CorePrivate/GuiPrivate/WidgetsPrivate）已接入；AUTORCC 显式开启（qt_standard_project_setup 不含）；qwindowkit 目录作用域关 clang-tidy（390KB 样式源 OOM 实测）。主程序 main.cpp 应用 FluentUI3Style + 浅色 + Fluent 配色（[ui] 驱动），像素取证浅色生效。
- [x] C4.2 窗口骨架：FluentWindowFrame（frameless，标题栏为 chrome header，含主题/置顶/搜索、系统三键）+ 左侧 ExWinUINavigationView（主菜单=主页/指令；页脚=设置/关于，设置在上；默认展开；Segoe Fluent Icons）+ QStackedWidget 四页 + 底部统计栏（视频/AI/遥测/TCP/SSH 五组）。
- [x] C4.3 主页迁移（数据链路不变）：中上视频（VideoGLWidget 平移）+ 右上 Quick3D 姿态 + 右下传感器卡（横滚/俯仰/航向/温湿度/电池电压+SOC 或"待标定"/DYP-RD 六态/数据源与新鲜度）+ 中下控制区（10 舵机 + 6 推进器滑条、horizontal 基准组、红色紧急停机 + 独立橙色紧急上浮固定区）+ 顶部告警摘要条（三级配色 + 合并计数）；QSplitter 三层可调宽、紧急区/姿态列不可折叠。指令/设置/关于为 Phase 5 占位页。
- [x] C4.4 权限矩阵接线：SafetyStateModel.stateChanged -> 控件置灰/隐藏（horizontal on 切基准滑条、断线全锁、estop 已触发/紧急进行中文案）+ ControlViewModel 三态标签（目标/确认/未知）+ estop 单帧直发 + emergency 二次确认（ExMessageBox，[ui] 开关）；TCP 生命周期全接线（连接/权威状态事件/ACK/超时 -> 安全状态机 + VM 回填）；TcpClient.requestSent 增补 payload（VM 通道回填）；首连失败也广播离线（标签不再滞留"连接中"前文案）。
- 验证：视觉走查（导航/布局/紧急按钮/告警条/五组状态栏齐备）；a11y 树取证（断线态舵机/推进器 disabled、紧急按钮"无法下发"disabled——权限矩阵真实生效）；带流冒烟（D3D11 管线 + DirectML 就绪）；**优雅关闭退出码 0x0**（完整逆序停机日志，TD-8 回归通过）；Debug/Release 双构建零告警；95/95 测试全绿；GBK 0 损坏。

### Phase 5 —— 指令页 + 设置页 + 关于页 + 删除 SSH ✅ 已完成（2026-08-30）
- [x] C5.1 指令页（`src/widgets/CommandPageWidget.*`）：系统/安全/模式/舵机/推进器/传感器七组参数化表单覆盖注册表全部 Request 函数；estop/emergency 复用主页确认与单帧链路（信号转发）；高级原始入口=注册表枚举组合框+空载荷（禁 shell）；结果表（时间/命令/Seq/状态/详情五列，容量 300 防涨）显示请求中/成功(耗时)/失败(错误码)/超时/离线拒绝（"未发送/链路不可用"实证）；数据响应解码展示（servo/propeller 列表，否则十六进制截断）；离线门控全部发送入口。
- [x] C5.2 设置页（`SettingsPageWidget.*`）：主题（浅/深）与配色（Fluent/Teams）**即时切换**（qApp `_q_colorscheme`/`_q_themestyle` + 全窗口 repolish + 标题栏 setThemeDark；深色与 Teams 均实证生效）并写回 ini 持久；运行参数摘要（网络/TCP/视频/AI/配置文件路径，只读）；打开配置目录（AppConfig 新增 iniPath()）。关于页（`AboutPageWidget.*`）：软件名/版本占位"未配置（待提供）"（不编造）/协议版本"草案 v1"/构建信息（Debug|Release + Qt 版本 + 位宽，事实性）/界面库与许可证。
- [x] C5.3 控制区竖直滑条 + 输入框：10 舵机 + 6 推进器改为**竖直滑条**（刻度 + 通道号），上方"目标｜确认 ?"三态标签，下方 QLineEdit（QIntValidator 值域、空值/非法字符回显当前目标、越界经 VM 钳位、回车与失焦双提交、粘贴同路径、滚轮被 eventFilter 拦截防误触）；被拒回滚时滑条与输入框同步恢复；horizontal on 基准组保持（6 路单帧）。
- [x] C5.4 删除 SSH 运行时：删 SshClient.*、ControlPanelWidget.*（文件级）；MainWindow 清 include/成员/状态栏标签/停机序列（状态栏改四组：视频/AI/遥测/TCP）；CMake 移除 libssh 发现块与 SALACIA_SSH_LINK（不再依赖 F:/libssh）；ini 删 ssh_* 六键（注释迁移说明）；AppConfig 删 SSH 成员/getter/读取块/摘要字段；**全库 grep ssh/libssh 零残留**（文档除外）。closeEvent 逆序变 视频→遥测→TCP→AI。
- [x] C5.5 文档：README 更新（SSH 移除清单+迁移、界面、测试、架构）；新增 `docs/WINDOWS_A35_INTERFACE.md`（帧格式/31 函数表/汇总帧/状态事件/错误码草案/行为契约/待确认 10 项）。
- 修复（验证中发现）：**TCP 重连退避整数溢出**（retry 高时 base×2^n 超出 int → 负间隔 QTimer 告警刷屏）——幂次钳位 20 + qint64 计算后封顶；AppConfig::iniPath 丢失赋值补回；logSummary 删 SSH 参数时丢闭括号修复；UI 库命名空间前置声明修正（FluentTitleBar 全局/页面类 salacia）；AboutPage ↔ 字符 GBK 不可表示改写。
- 回归：Debug/Release 双构建零告警（LNK4217 亦清零）；95/95 测试绿；GBK 0 损坏；指令页/设置页/主题切换（深色+Teams 像素级实证）/关于页 UIA 走查；优雅关闭完整停机序列（遥测→TCP→AI→主窗口）退出干净。

### Phase 6 —— 集成验收 ✅ 已完成（2026-08-30）
- [x] C6.1 全量 mock 回归（`tests/test_phase6.cpp`，新目标 salacia_tests_phase6，10 用例）：
  - 100Hz 长时：SALACIA_LONG_TEST=1 时 **280 秒 28000/28000 帧零丢失**（QtTest 单函数 300s 硬超时限制取 280s；短模式 12s 1150/1200≈96%）；事件循环全程响应（UI 不阻塞红线）；
  - 断连风暴：3 轮断开-退避重连全恢复（轮询连接态判定，前置稳定期防竞态），风暴后链路可用；
  - **权限矩阵端到端**：mock StateEvent（safe/horizontal/estop/emergency/恢复 五态+初态）→ TcpClient eventReceived → SafetyStateModel.applyAuthoritative → canServoIndividual/canThrusterIndividual/canBaseSlider 逐行断言（与 MainWindow 同接线方式）；
  - 错误码→失败映射：按函数定制 errCode=7(safety)，ACK 透传错误码、不误报超时；
  - StateEvent 管道：掩码 (safe|estop) 解码正确。
- [x] C6.2 性能测量（记录于测试 qInfo）：
  - 100Hz 接收有效吞吐 **93.9fps**（5s 窗 498 帧；mock 突发式供给，客户端跟满）；
  - **estop 插队延迟 11-19ms**（积压 40 条普通指令时，红线 ≤100ms）；
  - 控制出队平均延迟 **8.3-28ms**（30 轮含回环，红线 ≤50ms）。
- [x] C6.3 验收：Debug/Release 双构建零告警；**全量 9 套件 105/105 用例绿**（10+14+12+10+14+8+14+13+10）；GBK 0 损坏；Release 冒烟（四页 UIA 走查 + 优雅关闭完整停机序列）。
- 工程修复（验证驱动）：mock 流定时器改突发式（Windows 定时器合并 ~15.6ms 使 >64Hz 单帧定时不可达，20ms 周期×2 帧实现均值 100Hz）+ Qt::PreciseTimer；mock 扩展（StateEvent 注入/按函数错误码/帧到达时刻记录）；controlvm 测试 qWait 80→250ms（50ms 节拍 + 负载余量，10 连跑稳定）；长时 310→280s（QtTest 超时）。
- 交付物清单（提示词 §十六）核对：vibeplan.md（本文件，Phase 0-6 全记录）✅；接口说明/函数表/错误映射（docs/WINDOWS_A35_INTERFACE.md）✅；config.ini 完整键表（README + ini 注释）✅；源代码与迁移说明（README SSH 移除清单）✅；单元测试 + mock A35（tests/，9 目标 105 用例）✅；构建/运行/连接/排障说明（README + HANDOFF.md）✅；SSH 移除清单（README）✅；100Hz 接收/发送延迟测量（本 Phase 数据）✅；待确认 A35 接口/4S 标定清单（docs §7 + vibeplan §3.3）✅。


- C6.1 全量 mock 回归（含 100Hz 长时、断连风暴、错误码映射、权限矩阵全状态）。
- C6.2 性能：100Hz 接收 CPU/内存、滑条→报文延迟、estop 插队延迟测量记录。
- C6.3 Debug+Release 双构建零新告警 + 交付物清单核对（§十六）+ 待确认项终版。验收：全部 ctest 绿 + 人工验收单。

---

### Phase 11 - UI 精修 II（2026-08-31 完成）
- 窗口尺寸保持：告警展开/收起（toggleExpanded 中保存恢复 normalGeometry + panel maxHeight 260 限高）与菜单栏折叠（navToggle 恢复非最大化矩形，最大化时不调 setGeometry 保持全屏状态）
- safe/horizontal SwitchButton：QCheckBox + setProperty("isSwitchButton", true)（Gallery SwitchButton 样式）；各占一行（标签 180px + 开关 + ProgressRing + 状态文字）；四态绑定：On=checked/Off=unchecked/Pending=checked+ring 可见+文字"请求中..."/Unknown=置灰 setEnabled(false)+文字"状态未知"；请求失败经 SafetyStateModel 回滚权威值自动切回 Off 并更新告警
- 关于页：Gallery AboutProjectWidget 卡片式布局（QFrame::StyledPanel + aboutCard + 18pt 粗体标题 + Mid 色副标题 + 11pt 富文本内容）；AboutPageWidget 继承改 QFrame
- 告警 ExInfoBar 弹窗：ExInfoBarHost::setDefaultTarget(this) + alarmsChanged -> showInfoBar(severity, title, message, TopLeft)，超时 -1 使用 defaultHost 默认值（4500ms）；标题按级别显示 错误/警告/信息（Gallery 默认配色 Informational/Warning/Error）；弹出同时更新告警栏摘要（同一 alarmsChanged 处理链）
- ExProgressRing（exwidgets/exprogressring.h/.cpp）复制入 src/ui（GBK 转换 + ui CMake 源清单）；ProgressRing 用于请求中指示（QProgressBar range(0,0) 22x22）
- 回归：Debug/Release 零告警；105/105 测试绿；GBK 0 损坏；优雅关闭完整停机；UIA 走查主页布局正常

### Phase 10 - UI 精修与告警中心（2026-08-31 完成）
- 标题栏清理（搜索/主题/置顶 hide）；导航折叠按钮；图标乱码修复（QChar 码点构造，根因 GBK 源 uXXXX 转义破坏窄字符串）
- 共享 ControlAreaWidget（舵机/推进器竖直滑条+输入框+基准切换+权限门控+紧急固定区），主页与指令页复用
- 指令页 safe/horizontal 开关按钮（权威态四态绑定，点击反向下发）
- 设置页：Win11 风格强调色（24 预设+自定义，QPalette Accent/Highlight 即时生效+写回 ini）；12 项运行参数实时编辑（原子 setter）+ 保存入 ini
- AlarmBarWidget 完整告警中心：摘要+展开列表（7 列）+三级筛选+A35 AlarmEvent+对端时间戳+markRecovered 恢复+合并升级
- AppConfig 原子化 12 项实时参数；AlarmModel 容量/合并窗口实时读配置
- 回归：105/105 绿；双构建零告警；优雅关闭；UIA 走查通过

## 11. Phase 0 结论

审计完成，本文件即 Phase 0 交付物。未修改任何业务代码、配置与构建脚本；未执行任何 git 写操作。工作区仅含 2 个未跟踪需求文档（保留）。

---

# 第二轮优化（Phase 12–19）

> 日期：2026-09-01
> 依据：`docs/VibePrompt.md`（第二轮优化提示词，已复制入项目；与一轮文档/HANDOFF 冲突时以二轮提示词为准）
> 纪律：每 Phase 完成后停止等待"编译通过/继续"；每次修改及时 commit（范围限定 Salacia_Terminal/，不 push 不 PR）；源码 GBK、Markdown UTF-8；Windows 与 M33 双盲，A35 是唯一协议转换与状态权威；本任务不改 A35/M33/设备树/外部 SDK；代码内禁硬编码、禁绝对路径（Windows 系统文件除外）。

## R.1 一轮终态与二轮差距

| # | 一轮现状 | 二轮目标 | 涉及 |
|---|---|---|---|
| D1 | Estop 32B 载荷（10 舵机+6 推进器零值） | Stop/Estop/Emergency 均空载荷、只置零六路推进器、绝不携带/操作舵机 | WireConstants/FunctionRegistry/ControlViewModel/测试/文档 |
| D2 | 舵机 1–10 / 推进器 1–6 两套 1 基 ID，`id-1` 兼任索引 | 舵机 wire 0–9（UI 1–10）；垂直 10–13；水平 14–15；UI 索引/显示编号/wireId 三分离 | WireConstants/ControlViewModel/ControlAreaWidget |
| D3 | SafetyStateModel 单一 `ModeState::Pending`；estop/emergency 布尔 | `PendingSwitchState`×7（authoritative/displayedTarget/pendingSeq/pendingDirection/rollbackState/pendingTimestamp） | SafetyStateModel |
| D4 | StateEvent 0x0102 单字节 4 位掩码 | StateEventV2 0x0104（u8 version + u16 mask，9 位），legacy 0x0102 保留不改位义 | WireConstants/FunctionRegistry/MainWindow/MockA35 |
| D5 | `canServoIndividual()` 依赖 safe/horizontal/estop/emergency | 舵机权限仅：连接+可发送+参数合法+无同通道 Pending；任何推进器模式不得置灰舵机 | SafetyStateModel/ControlAreaWidget |
| D6 | 主页"推进器 1–6"六滑条 | 垂直/水平两组；姿态稳定 ON=每组一条基准滑条；OFF=按各组同步状态单条/多条 | ControlAreaWidget |
| D7 | 无同步开关 | 垂直/水平 Synchronization 开关（主页推进器模块+指令页模式区，同一状态模型） | 新 funcId 0x0024–0x0027 |
| D8 | 无 Stop/Move 三级开关 | 推进器总使能/垂直推进使能/水平推进使能（ON=Move，OFF=Stop 锁存） | 新 funcId 0x0013–0x0017 |
| D9 | "Horizontal 姿态补偿"语义混淆（易误解为水平推进器） | 更名"姿态稳定（Horizontal）"=Roll/Pitch 自动调平；Safe 单向联动 | UI/SafetyStateModel |
| D10 | 指令页无视频 | VideoFrameHub 共享最新帧，指令页左上小视频，单管线单端口 | 新 src/video/VideoFrameHub |
| D11 | 菜单折叠/告警展开用 topLevel `setGeometry` 恢复几何 | 禁 resize/adjustSize/setFixedSize/showNormal/showMaximized/setGeometry；仅内部布局压缩 | MainWindow/AlarmBarWidget |

## R.2 协议设计定稿

### R.2.1 funcId 分配（集中 WireConstants::Func + FunctionRegistry 全表 + docs/WINDOWS_A35_INTERFACE.md）

| 函数 | funcId | 变更 | 载荷 | 优先级 |
|---|---|---|---|---|
| StopAll | 0x0010 | 复用原 Stop，语义=六路推进器置零并进入停止状态 | 空 | 2 |
| Emergency | 0x0011 | 语义变更：同样仅六路推进器置零，不上浮 | 空 | 1 |
| Estop | 0x0012 | 载荷 32B→空；与 Stop 执行结果完全相同，仅优先级/GUI 告警等级/日志事件类型不同 | 空 | 0 |
| MoveAll | 0x0013 | 新增 | 空 | 2 |
| StopVertical / MoveVertical | 0x0014 / 0x0015 | 新增 | 空 | 2 |
| StopHorizontal / MoveHorizontal | 0x0016 / 0x0017 | 新增 | 空 | 2 |
| VerticalSynchronizationOn/Off | 0x0024 / 0x0025 | 新增 | 空 | 5 |
| HorizontalSynchronizationOn/Off | 0x0026 / 0x0027 | 新增 | 空 | 5 |
| BaseValueVH | 0x0051 | 新增；0x0050 弃用（注册表保留标记 deprecated，代码不再使用） | 2×i16：垂直基准、水平基准 | 5 |
| StateEventV2 | 0x0104 | 新增；0x0102 legacy 保留原位义 | u8 version=2 + u16 mask | 事件 |

优先级常量：`kPriorityEstop=0`、`kPriorityEmergency=1`、`kPriorityStopMove=2`、`kPriorityNormal=5`；紧急队列（priority<5）改为按优先级稳定排序插入、永不丢弃，保证 `Estop > Emergency > Stop/Move > 普通控制` 插队次序。

### R.2.2 StateEventV2（0x0104）

```
u8  version = 2
u16 mask（小端）：
  bit0 safe                      bit1 attitudeStabilization
  bit2 globalStopped             bit3 verticalStopped
  bit4 horizontalStopped         bit5 verticalSynchronization
  bit6 horizontalSynchronization bit7 estop
  bit8 emergency
```

- stopped 位=1 表示该组停止锁存；UI"使能"开关显示取反值（globalStopped=0 → 总使能 ON）。
- 未知位（bit9–15 置位）整帧拒绝，与 legacy 行为一致。
- legacy 0x0102 继续可解码（旧 4 位含义不动），作为兼容回退；主链路切换 V2，MockA35 默认发 V2。

### R.2.3 执行器 ID（UI 索引 / 显示编号 / wireId 三分离）

| 类别 | wireId | UI 编号 | 标签 |
|---|---|---|---|
| 舵机×10 | 0–9 | 1–10 | 舵机1（CH0）… 舵机10（CH9） |
| 垂直推进器×4 | 10–13 | 1–4 | 垂直1（CH10）… 垂直4（CH13） |
| 水平推进器×2 | 14–15 | 1–2 | 水平1（CH14）、水平2（CH15） |

- 拓扑常量集中 WireConstants：`kServoCount=10`、`kServoIdFirst=0`、`kVerticalCount=4`、`kVerticalIdFirst=10`、`kHorizontalCount=2`、`kHorizontalIdFirst=14`、`kThrusterCount=6`、`kIdBroadcast=0xFF`。
- ServoSet/Get/Mid：id∈[0,9]；"全部"统一 `kIdBroadcast=0xFF`（ServoSetAll 仍独立 funcId 带 u16 angle）。PropellerSet：id∈[10,15]；PropellerStop：0xFF=全部。非法 ID 编码返回空 QByteArray（拒绝发送）。
- AppConfig `[control] servo_count/thruster_count/id_base` 弃用：读取时 Logger 告警，拓扑以 WireConstants 为准（ini 键保留兼容不删）。
- Windows 只把这些 ID 视为 Windows↔A35 协议规范 ID，不引用不解析 M33 协议。

## R.3 PendingSwitchState（SafetyStateModel 重写核心）

7 个事务开关共享：Safe、姿态稳定（AttitudeStabilization）、推进器总使能、垂直使能、水平使能、垂直同步、水平同步。estop/emergency 仍为 StateEventV2 权威布尔（无本地事务）。

```
struct PendingSwitchState {
    ModeState authoritative;    // 最近一次 A35 确认（On/Off/Unknown）
    bool     pending;           // 事务进行中
    bool     displayedTarget;   // Pending 期间立即显示的目标
    quint16  pendingSeq;
    int      pendingDirection;  // ToOn / ToOff
    bool     rollbackState;     // 发起时权威值（回退目标）
    qint64   pendingTimestampMs;
};
```

事务规则（7 开关一致，逐条对应二轮提示词 §三）：
1. 点击 → displayedTarget=目标、pending=true、禁重复点击、ProgressRing 显示；
2. 成功 ACK → authoritative=目标、pending=false、Ring 隐藏；
3. 失败 ACK/超时 → 回退 displayedTarget=rollback、pending=false、Ring 隐藏、窗口级 ExInfoBar + AlarmModel + 日志（funcId/SEQ/目标状态/错误原因）；
4. 断线 → pending 全清、authoritative=Unknown（显示"状态未知"，保守禁用推进器；舵机按 §R.6 权限不受影响）；
5. StateEvent 先于 ACK 到达 → 以 StateEvent 为权威、清对应 pending；
6. 成功 ACK 后 StateEvent 给出相反状态 → StateEvent 覆盖 + 协议状态不一致告警。

## R.4 Safe ↔ 姿态稳定联动

- GUI 更名"姿态稳定（Horizontal）"；业务含义=ROV Roll/Pitch 自动调平，不代表 CH14–CH15 水平推进器。
- Safe 定义（A35 侧保护策略，Windows 不做安全权威）：Safe 不是 Stop/Emergency，不控制舵机，不自动置零推进器；在姿态稳定基础上执行边界检查/限幅/拒绝，最终裁决由 A35 负责。
- 联动规则（单向）：
  - SafeOn 仅发一条业务级 0x0020，Windows 不自行拼接两条命令；
  - GUI 将 Safe 与姿态稳定同时显示为目标 ON（双 Pending、双 Ring）；
  - A35 原子保证姿态稳定开启后再确认 Safe 成功；成功双 ON；失败双回退到原权威状态；
  - Safe ON 期间姿态稳定开关保持 ON+禁用+说明"Safe 模式要求姿态稳定开启"；SafeOff 只关 Safe，姿态稳定保持 ON 恢复可操作；
  - Safe ON 不锁死舵机与推进器提交：舵机始终可操作；推进器仍可提交控制请求，A35 限幅/拒绝时按返回结果更新确认值、回退并提示；
  - 非法权威组合 `Safe=ON + Stabilization=OFF` → 锁定推进器控制 + 高等级告警，Windows 端不悄悄修正。

## R.5 推进器布局（纯逻辑判定函数，UI 只做显隐）

```
姿态稳定 ON  → 垂直组 1 条基准滑条 + 水平组 1 条基准滑条（BaseValueVH 双值）
姿态稳定 OFF → 每组按自身 Synchronization 独立判定：
    垂直同步 ON → 垂直组 1 条同步滑条；OFF → 垂直1–4 四条独立滑条
    水平同步 ON → 水平组 1 条同步滑条；OFF → 水平1–2 两条独立滑条
```

- 姿态稳定 ON 时两组同步开关仍显示权威状态、允许切换（事务正常），布局保持"一组一条"并显示提示"姿态稳定模式下使用统一基准，Synchronization 状态将在退出姿态稳定后决定布局"。
- 所有滑条保留 目标值/已发送值/A35 确认值 三态；限频、松手立即冲刷、断线不重放规则不变。
- Unknown（同步/稳定状态未知）→ 保守显示独立滑条并禁用推进器控制，不显示为已关闭。

## R.6 Stop/Move 三级使能与权限矩阵

- 开关：推进器总使能（MoveAll/StopAll）、垂直推进使能（MoveVertical/StopVertical）、水平推进使能（MoveHorizontal/StopHorizontal）；ON=允许运动，OFF=停止锁存，不暴露"Stop ON"反向语义；主页与指令页展示并实时同步（同一 SafetyStateModel）。
- 推进器实际可操作 = TCP 已连接 + 权威状态已知 + 总使能 ON + 对应分组使能 ON + 相关开关不处于 Pending。
- 总使能 OFF 或 PendingToOff → 全部推进器滑条与输入框立即置灰；垂直/水平分组开关保留权威显示但禁止发起 Move；舵机保持可操作。
- 总使能重新 ON → 仅恢复未被分组 Stop 锁定的组；不自动恢复或重发旧推进器目标（清逐路待发）。
- 垂直使能 OFF 只置灰垂直组；水平使能 OFF 只置灰水平组。
- Stop/Move 不改变 Safe、姿态稳定、Synchronization、舵机状态。
- 舵机可操作条件（与一切推进器模式解耦）：TCP 连接存在 + 命令可发送 + 通道/角度参数合法 + 当前通道无冲突 Pending。

## R.7 视频复用 VideoFrameHub

- 新增 `src/video/VideoFrameHub`：单帧槽（互斥锁 + 帧拷贝发布），GStreamerPipeline 显示 appsink 回调发布最新 BGRA 帧，覆盖旧帧不积压，GUI 线程永不阻塞；主页与指令页两个 VideoGLWidget 实例各自按 33ms 节拍快照渲染，不竞争 RingBuffer。
- AI 分支 RingBuffer 链路不动；仅一个 GStreamerPipeline、一个 UDP 视频接收源、一套解码流程（不变式）。
- 指令页左上小视频：独立小尺寸 VideoGLWidget 实例（不共享 QWidget 父对象）、保持宽高比不拉伸、检测框按归一化坐标缩放、无画面显示离线/等待提示；页面切换不停止/重启/重建管线。

## R.8 窗口尺寸硬性要求

- 菜单折叠（MainWindow）与告警栏展开/收起（AlarmBarWidget）删除全部顶层窗口 setGeometry/resize 类调用；只允许改变窗口内部布局（告警面板限高 + 内容区 stretch 压缩）。
- 验收：普通窗口展开/收起前后顶层 geometry 不变；最大化状态恒 `isMaximized()==true`；不出现恢复普通窗口、窗口跳动、尺寸逐次增长；不依赖固定屏幕坐标（DPI/多屏安全）。

## R.9 Phase 计划（每 Phase：实现 → 测试 → Debug 构建 0 新警告 → commit → 停止等编译确认）

| Phase | 内容 | 主要交付 |
|---|---|---|
| 12 | 计划文档 | 本节总设计 + docs/VibePrompt.md 复制入库 |
| 13 | 协议层 | WireConstants/FunctionRegistry：新 funcId、ID 拓扑常量与映射助手、StateEventV2/BaseValueVH 编解码、Estop 空载荷（删 32B）、Stop/Move 优先级、紧急队列按优先级排序；受影响调用点编译适配；test_wirecodec/test_registry 更新 |
| 14 | 状态模型 | SafetyStateModel 重写（R.3/R.4/R.6 全部规则与权限）；MainWindow 消费 StateEventV2；MockA35 增加 V2 注入并设默认；test_safetystate 重写扩展 + test_phase6 权限矩阵更新 |
| 15 | ControlViewModel | 垂直/水平分组通道 + wire 映射、双基准值、Stop/Move 请求、锁存置灰与不重放、estop 空载荷调用点；布局判定纯逻辑；test_controlvm 扩展 |
| 16 | UI | ControlAreaWidget 重构（垂直/水平分组、布局动态切换、3 使能+2 同步开关、舵机标签与权限解锁）；指令页模式区 7 开关（同一模型）；Pending Ring/回退 ExInfoBar/AlarmModel/日志接线 |
| 17 | 视频 | VideoFrameHub + 指令页左上小视频 + hub 单元测试（双读者同帧/丢旧不积压/不阻塞） |
| 18 | 窗口 | 几何修复 + 新增 test_windowgui GUI 测试套件（普通/最大化/重复操作） |
| 19 | 文档与验收 | WINDOWS_A35_INTERFACE.md 全面改版、README、HANDOFF 当前状态、DELIVERY_REPORT.md；Debug+Release 全量构建 + 全部测试汇总；证据等级注明"Windows 侧和 Mock A35 通过，实机未对接" |

## R.10 测试计划（对齐二轮提示词 §十三）

- 编码：Servo ID 0–9、Vertical CH10–13、Horizontal CH14–15、非法 ID 拒绝、Stop/Estop/Emergency 载荷不含 Servo、新 funcId 唯一且注册表完整。
- Safe/姿态稳定：SafeOn 双 Pending 目标 ON、成功双 ON、失败双回退、Safe ON 不能关姿态稳定、SafeOff 不关姿态稳定、Safe 不锁舵机、非法组合 Safe ON+Stab OFF 触发告警与推进器锁定。
- SwitchButton 事务（7 开关逐个）：PendingToOn/PendingToOff、成功提交、NACK 回退、超时回退、断线回退 Unknown、StateEvent 先于 ACK、ACK 与 StateEvent 冲突、ProgressRing 显隐。
- 推进器布局：稳定 ON=垂直 1+水平 1；稳定 OFF+双同步 OFF=4+2；仅垂直同步 ON；仅水平同步 ON；总使能 OFF 全灰；分组使能 OFF 只灰对应组；任何推进器模式不影响舵机。
- 视频：主页/指令页共享单管线；两视图同时获最新帧；页面切换不重启管线；不竞争弹出同一 RingBuffer 帧；小画面保持比例。
- 窗口（GUI 测试）：普通窗口展开/收起菜单后 geometry 不变；告警栏同；最大化操作后仍最大化；重复操作无尺寸漂移。

## R.11 风险与回滚

| 风险 | 缓解 |
|---|---|
| StateEventV2 与 A35 实机后续实现不一致 | 常量集中 WireConstants；version 字段前向兼容；legacy 0x0102 保留可退回；docs 待确认清单持续维护 |
| 紧急队列排序引入发送回归 | Phase 13 单测覆盖插队次序（Estop>Emergency>StopMove>普通）；Phase 6 estop 插队延迟性能用例回归 |
| 布局动态切换破坏滑条三态 | 布局判定纯函数化，test_controlvm 先行；UI 只做显隐不改三态数据 |
| 双视频渲染 GL 上下文开销 | 小视频独立实例仅新帧 update；页面隐藏时跳过绘制 |
| GBK 转换破坏中文源文件 | 沿用一轮纪律：新/改文件统一转 GBK + iconv 往返校验 |
| 窗口几何修复导致内容挤压 | 告警面板 maxHeight + 内容区最小尺寸约束；GUI 测试断言 |

回滚：每 Phase 一个 commit 保持可编译，按 commit 粒度回退。
