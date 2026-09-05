# Salacia Terminal —— ROV 水下机器人岸上终端

Windows 桌面端上位机（Qt Widgets），为 STM32MP257 水下机器人提供：

| 功能 | 说明 |
|------|------|
| 实时视频 | RTP/H264 over UDP（1080p 主用 / 720p），D3D11 硬解 + OpenGL 渲染，动态分辨率切换，总线错误/断流自动重建；主页与指令页左上小画面经 VideoFrameHub 共享单管线最新帧（零拷贝） |
| AI 识别 | ONNX Runtime（YOLO 系）推理，检测框实时叠加（双视图同源归一化坐标），GPU 直通缩放喂帧，后端自动探测：CUDA/TensorRT → DirectML → OpenVINO → CPU |
| 舱体遥测 | 20Hz UDP 遥测（MPU6500 六轴 + 舱内温湿度 + 电池电压），Mahony 姿态解算，表单 + Quick3D 三维模型实时显示 |
| 执行机构遥控 | Windows↔A35 TCP 二进制帧（CRC16/seq-ACK/双优先级队列/指数退避重连/迟到响应丢弃）：10 舵机（wire 0-9）+ 垂直推进器 CH10-13 / 水平推进器 CH14-15 分组控制，三态滑条（目标/已发送/A35 确认） |
| 模式与安全 | Safe↔姿态稳定单向联动、推进器总使能/垂直/水平三级 Stop-Move 锁存、双 Synchronization、StateEventV2 权威状态（PendingSwitchState 事务：目标显示/ProgressRing/NACK/超时/断线回退/协议不一致告警）；Stop/Estop/Emergency 仅推进器置零、不操作舵机 |

## 环境

| 依赖 | 版本 / 路径（本机实测） |
|------|------------------------|
| 编译器 | VS2026 Insiders MSVC 14.51（`F:/Microsoft Visual Studio/18/Insiders`） |
| CMake | ≥ 3.20（Ninja 生成器） |
| Qt | 6.11.1 msvc2022_64（`F:/Qt/6.11.1/msvc2022_64`），组件：Core/Gui/Widgets/Quick/Quick3D/QuickWidgets/Network/Concurrent/OpenGL/OpenGLWidgets |
| GStreamer | 1.28.6 MSVC x86_64 运行时+开发包（`F:/gstreamer/1.0/msvc_x86_64`），`bin` 需在 PATH |
| ONNX Runtime | 1.24.4 C++ 发行版三变体（`F:/onnxruntime/{directml,gpu,cpu}-1.24.4`） |

字符集约定：**C++ 源码 GBK（多字节）**，`config/*.ini`、`.qml`、Markdown 为 UTF-8。

## 构建

```bat
:: 1) 打开 VS2026 开发者命令提示（x64），确认 Qt 在 CMake 前缀路径
set QTDIR=F:/Qt/6.11.1/msvc2022_64

:: 2) 配置（预设已含 QTDIR；默认 ONNX 变体为 directml）
cmake --preset Release-x64
:: RTX 独显平台切换 CUDA/TensorRT 变体：
:: cmake --preset Release-x64 -DONNXRUNTIME_ROOT=F:/onnxruntime/gpu-1.24.4

:: 3) 构建
cmake --build out/build/release
```

依赖路径均可用 CMake 缓存变量覆盖：`GSTREAMER_ROOT`、`ONNXRUNTIME_ROOT`。
构建输出自动拷贝 `config/` 与所选变体 ORT DLL 至 exe 目录（规避 System32 旧版 onnxruntime.dll 遮挡）。

## 运行

工作目录需包含（构建后已就位）：

```
Salacia_Terminal.exe
config/app_config.ini     # 全部可调参数（禁止硬编码）
models/model.onnx         # 识别模型，自备放置（不入库）
logs/                     # 运行日志 + 崩溃转储 salacia_crash_*.dmp（自动生成）
```

所有参数见 `config/app_config.ini`（端口/解码器/模型路径/输入尺寸/置信度/NMS IoU/推理后端/SSH/电池折算/PWM 量程/Mahony 增益），修改后重启生效。

## 部署打包

```bat
:: 1) windeployqt（--qmldir 必需：Quick3D 姿态视图依赖 QML 模块）
F:/Qt/6.11.1/msvc2022_64/bin/windeployqt --release --qmldir src/qml Salacia_Terminal.exe

:: 2) 拷贝 ORT 变体 bin（onnxruntime.dll + DirectML.dll）
xcopy F:/onnxruntime/directml-1.24.4/bin\*.dll . /Y

:: 3) GStreamer 运行时为前置条件：目标机安装 1.24+ 并入 PATH，
::    或整体拷贝 gstreamer/1.0/msvc_x86_64 并设置 GST_PLUGIN_PATH
```

目标机性能基线（Iris Xe 核显 + DirectML，720p@30 推流 + 20Hz 遥测 + 推理并发实测）：
视频 29.9fps 零丢帧、单帧推理 3ms、整机 CPU 约 1.5%，退出码干净 0x0。

## SSH 运行时移除清单（Phase 5）

原经 SSH（libssh）下发 `pwm <id> <us>` 的遥控通道已于本轮**完全移除**，控制统一走
Windows↔A35 TCP（见 docs/WINDOWS_A35_INTERFACE.md）：

- 删除 `src/communication/SshClient.h/.cpp`、`src/widgets/ControlPanelWidget.h/.cpp`
- CMake：libssh 发现/链接块、`SALACIA_SSH_LINK`、相关注释全部移除（不再依赖 `F:/libssh-0.12.2-msvc`）
- ini：`[rov] ssh_host/ssh_port/ssh_user/ssh_password/ssh_key_path/ssh_reconnect_sec` 作废删除；
  AppConfig 对应 getter 移除；状态栏 SSH 标签移除
- 迁移：舵机/推进器控制改经 `[tcp]` 通道（`set servo/propeller` 系列），无回退路径

## 板端对接协议

### 1. 视频（板端 CamStream → 终端）

- RTP/H264 over UDP 单播 → 岸机 `[network] host_ip:rtp_port`（默认 192.168.137.1:5000，ICS NAT 有线模式），动态 PT=96（clock-rate 90000），MTU 1400
- 分辨率 1920×1080（主用）/ 1280×720，可随流动态切换
- SPS/PPS 每 1s 带内重发、关键帧间隔 1s → 接收端迟入 ≤1s 自动同步，无需 RTSP
- 板端启动示例（目标地址 = 主机 IP）：`camstream_1080p /dev/video1 192.168.137.1 5000 4000`（1080p@4Mbps）/ `camstream_720p /dev/video1 192.168.137.1 5000 3000`；**板端默认目标是 192.168.1.100，务必显式传主机 IP**
- **Windows 防火墙**：首次真机对接需放行终端入站 UDP——环路测试流量不过防火墙，真机板卡流量会被拦，典型症状为完全无画面。放行方式（管理员）：
  `netsh advfirewall firewall add rule name="Salacia Terminal" dir=in action=allow program="C:\...\Salacia_Terminal.exe" enable=yes`
- 终端侧接收已加固：2MB 接收套接字缓冲（抗 4Mbps 包突发防内核丢包）、绑定地址可配（`[network] host_ip`，留空=全接口）、5 秒无任何 RTP 包时在日志输出对接排查提示

### 2. 遥测 v2（板端 → 终端 `:5001`，20Hz）

UDP 定长 **50 字节**、小端、packed：

| 偏移 | 字段 | 类型 | 说明 |
|-----|------|------|------|
| 0 | magic | u16 | 0xA55A |
| 2 | version | u8 | 2 |
| 3 | flags | u8 | bit0 加速度有效 / bit1 陀螺有效 |
| 4 | sequence | u32 | 序号（回绕） |
| 8 | boardTimeMs | u32 | 板端毫秒时间戳 |
| 12 | accelMps2[3] | f32×3 | 机体系加速度 m/s² |
| 24 | gyroRadS[3] | f32×3 | 机体系角速度 rad/s |
| 36 | cabinTempC | f32 | 舱内温度 ℃ |
| 40 | cabinHumidityPct | f32 | 舱内湿度 %RH |
| 44 | batteryVoltage | f32 | 电池电压 V |
| 48 | crc16 | u16 | CRC16-CCITT-FALSE（初值 0xFFFF，多项式 0x1021），覆盖偏移 0~47 |

终端侧四重校验（长度/魔数/版本/CRC），坏包静默丢弃；1s 无包判定链路离线。
电量百分比 = (batteryVoltage − empty) / (full − empty) 线性折算（ini 可配，默认 4S 13.0~16.8V）。
参考实现：`src/communication/TelemetryPacket.h`（板端可直接移植 CRC 与结构体）。

### 3. 执行机构与模式控制（终端 → A35，TCP :7000）

Windows↔A35 二进制帧协议（小端 + CRC16-CCITT-FALSE + seq/ACK），完整定义见
**`docs/WINDOWS_A35_INTERFACE.md`（v2）**。要点：

- **双盲原则**：Windows 只理解 Windows↔A35 业务协议；A35 是唯一协议转换
  （RovControl/M33）与权威状态来源；终端不感知 M33 ASCII 协议与 RPMsg。
- **执行器 ID**：舵机 wire 0–9（UI 1–10）；垂直推进器 CH10–13；水平推进器
  CH14–15；UI 编号/wireId 分离，非法 ID 编码即拒绝。
- **模式语义**：Safe↔姿态稳定单向联动；Stop/Move 三级使能（总/垂直/水平，
  ON=允许运动 OFF=停止锁存）；垂直/水平双 Synchronization；Stop/Estop/
  Emergency 三者执行结果相同（仅推进器置零、不操作舵机、空载荷），
  差别只在优先级 `Estop(0) > Emergency(1) > Stop/Move(2) > 普通(5)`。
- **权威状态**：StateEventV2（0x0104，u8 version + u16 mask，9 位）为主链路，
  legacy 0x0102 保留兼容回退；开关事务（PendingSwitchState）以 ACK/事件为准，
  失败/超时/断线回退并告警。
- **证据等级**：以上均为 Windows 侧 + Mock A35 验证通过；**A35 实机尚未对接**，
  待确认清单见接口文档 §10。

## 界面（FluentUIStyle）

- 浅色/Fluent/FluentUI3 + frameless（QWindowKit）；左侧导航（主页/指令 + 页脚设置/关于，
  折叠/展开与告警栏展开只改内部布局，窗口尺寸/最大化状态不变）
- 样式库已复制进 `src/ui/`（MIT；qwindowkit Apache-2.0，许可证随附），**源码统一转 GBK**，
  无外部路径引用；构建需 Qt 私有头（CorePrivate/GuiPrivate/WidgetsPrivate）
- 主页：视频（中上）/ Quick3D 姿态 + 传感器卡（右列）/ 控制区（中下：10 舵机
  竖直滑条 + 垂直推进器/水平推进器分组 + 各组同步开关 + 使能开关列 +
  紧急停机固定区；姿态稳定 ON 时切基准滑条、同步 ON 时切单条同步滑条）；
  顶部告警摘要条（可展开，三级筛选）
- 指令页：左上角小尺寸实时视频（与主页共享单管线）+ 7 个模式事务开关
  （Safe/姿态稳定/总使能/垂直使能/水平使能/双同步，与主页同一状态模型）+
  控制区（舵机 5×2、垂直 2×2、水平 2×1 网格，水平滑条两行式器件格）+
  全部注册函数表单与受限原始入口（seq/ACK/耗时/错误结果表）
- 设置页：主题/配色/强调色即时切换（写回 ini）、运行参数实时编辑、打开配置目录

## 测试

```bat
cd out\build\debug
salacia_tests_appconfig.exe & salacia_tests_wire.exe & salacia_tests_registry.exe ^
  & salacia_tests_sensor.exe & salacia_tests_tcp.exe & salacia_tests_alarm.exe ^
  & salacia_tests_safety.exe & salacia_tests_controlvm.exe ^
  & salacia_tests_videohub.exe & salacia_tests_windowgui.exe & salacia_tests_phase6.exe
```
十一个套件 135 用例（配置校验/帧分帧/注册表与执行器 ID/TCP 集成含 mock A35/
传感器双源/告警/权限矩阵与开关事务/控制 VM 分组与不重放/视频帧 Hub/
真实窗口尺寸 GUI/Phase6 全量回归与性能）。测试 exe 为 WIN32 子系统，
建议 `-o 文件,txt` 或 ctest 方式运行。

## 架构要点（工业级多线程）

- 全部常驻任务 Worker-Object 模式（QObject + moveToThread，事件驱动），不重写 QThread::run()
- 视频解码线程 → AI 推理线程：无锁 SPSC RingBuffer（drain-latest 保低延迟）
- 传感器/检测共享状态：std::shared_mutex 读写锁 + std::atomic 链路标量，alignas(64) 防伪共享
- 跨线程 UI 更新一律信号槽显式 QueuedConnection；GUI 线程零阻塞 I/O
- 退出逆序：停网络 → 自终结 + 限时阶梯停工作线程 → 释放 GPU/ONNX 上下文

## 目录结构

```
Salacia_Terminal/
├── CMakeLists.txt          # 依赖发现 / GBK 与 clang-tidy 约束 / POST_BUILD 拷贝
├── config/app_config.ini   # 全量运行参数
├── docs/                   # WINDOWS_A35_INTERFACE.md（v2 接口权威）/ 二轮提示词
└── src/
    ├── main.cpp            # 入口：OpenGL RHI 前置、崩溃转储、逆序 shutdown
    ├── MainWindow.*        # 主界面装配（导航/告警栏/四页/状态栏/TCP 接线）
    ├── core/               # Logger AppConfig DataManager AlarmModel SafetyStateModel（PendingSwitchState×7）
    ├── control/            # ControlViewModel（分组通道/双基准/Stop-Move/不重放）
    ├── utils/RingBuffer.h  # 无锁 SPSC 环形缓冲（AI 通道）
    ├── video/              # GStreamerPipeline（自愈重建）/ VideoFrame / VideoFrameHub（最新帧发布层）
    ├── recognition/        # IModelInfer / OnnxInferEngine（动态 EP）
    ├── communication/      # WireConstants/WireCodec/FunctionRegistry（42 函数）/ TcpClient / TelemetryPacket / UdpReceiver
    ├── sensor/             # MPU6500Processor（Mahony）/ RovVizModel / SensorModel
    ├── widgets/            # VideoGLWidget / ControlAreaWidget / SwitchButtonWidget / CommandPageWidget / AlarmBarWidget 等
    └── qml/RovViz.qml      # Quick3D 三维舱体姿态
```
