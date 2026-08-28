# Salacia Terminal —— ROV 水下机器人岸上终端

Windows 桌面端上位机（Qt Widgets），为 STM32MP257 水下机器人提供：

| 功能 | 说明 |
|------|------|
| 实时视频 | RTP/H264 over UDP（1080p 主用 / 720p），D3D11 硬解 + OpenGL 渲染，动态分辨率切换，总线错误/断流自动重建 |
| AI 识别 | ONNX Runtime（YOLO 系）推理，检测框实时叠加，GPU 直通缩放喂帧，后端自动探测：CUDA/TensorRT → DirectML → OpenVINO → CPU |
| 舱体遥测 | 20Hz UDP 遥测（MPU6500 六轴 + 舱内温湿度 + 电池电压），Mahony 姿态解算，表单 + Quick3D 三维模型实时显示 |
| 执行机构遥控 | SSH 命令通道下发 16 路 PWM（10 舵机 + 6 推进器），滑条独立控制、50ms 合并节流、紧急停机 |

## 环境

| 依赖 | 版本 / 路径（本机实测） |
|------|------------------------|
| 编译器 | VS2026 Insiders MSVC 14.51（`F:/Microsoft Visual Studio/18/Insiders`） |
| CMake | ≥ 3.20（Ninja 生成器） |
| Qt | 6.11.1 msvc2022_64（`F:/Qt/6.11.1/msvc2022_64`），组件：Core/Gui/Widgets/Quick/Quick3D/QuickWidgets/Network/Concurrent/OpenGL/OpenGLWidgets |
| GStreamer | 1.28.6 MSVC x86_64 运行时+开发包（`F:/gstreamer/1.0/msvc_x86_64`），`bin` 需在 PATH |
| ONNX Runtime | 1.24.4 C++ 发行版三变体（`F:/onnxruntime/{directml,gpu,cpu}-1.24.4`） |
| libssh | 0.12.2 MSVC x64 静态库（`F:/libssh-0.12.2-msvc`），密码学后端复用 GStreamer 自带 OpenSSL 3.5 |

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

依赖路径均可用 CMake 缓存变量覆盖：`GSTREAMER_ROOT`、`ONNXRUNTIME_ROOT`、`LIBSSH_ROOT`。
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

:: 3) libssh 后端 OpenSSL（libssl-3-x64.dll / libcrypto-3-x64.dll，取自 GStreamer bin）

:: 4) GStreamer 运行时为前置条件：目标机安装 1.24+ 并入 PATH，
::    或整体拷贝 gstreamer/1.0/msvc_x86_64 并设置 GST_PLUGIN_PATH
```

目标机性能基线（Iris Xe 核显 + DirectML，720p@30 推流 + 20Hz 遥测 + 推理并发实测）：
视频 29.9fps 零丢帧、单帧推理 3ms、整机 CPU 约 1.5%，退出码干净 0x0。

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

### 3. 执行机构控制（终端 → 板端，SSH 通道）

板端提供 CLI：**`pwm <id> <us>`**（退出码 0 为成功，应答文本任意）

| id | 设备 | 面板输入 → PWM |
|----|------|----------------|
| 1–10 | 舵机 | 0~180° 线性映射 [servo_min_us, servo_max_us]（默认 500–2500） |
| 11–16 | 推进器 | −100~+100% 映射 [thruster_min_us, thruster_max_us]（默认 1100–1900，中位 1500） |

SSH 参数（端口/账号/密码或私钥/重连周期）见 `[rov]` 节，目标主机 = `[rov] ssh_host`（留空时取 `[network] board_ip`）。网络地址与端口统一在 `[network]` 节配置：`host_ip`（本机 UDP 绑定，也是板端推流目标）、`board_ip`、`rtp_port`、`telemetry_port`。
紧急停机按钮：推进器全部中位 + 舵机回中，立即下发绕过节拍。

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
└── src/
    ├── main.cpp            # 入口：OpenGL RHI 前置、崩溃转储、逆序 shutdown
    ├── MainWindow.*        # 主界面装配（视频区/控制坞/舱体状态坞/状态栏）
    ├── core/               # Logger（异步日志）AppConfig DataManager（共享状态）
    ├── utils/RingBuffer.h  # 无锁 SPSC 环形缓冲
    ├── video/              # GStreamerPipeline（自愈重建）/ VideoFrame
    ├── recognition/        # IModelInfer / OnnxInferEngine（动态 EP）
    ├── communication/      # TelemetryPacket / UdpReceiver / SshClient
    ├── sensor/             # MPU6500Processor（Mahony）/ RovVizModel
    ├── widgets/            # VideoGLWidget（OpenGL 渲染+检测框）/ ControlPanelWidget
    └── qml/RovViz.qml      # Quick3D 三维舱体姿态
```
