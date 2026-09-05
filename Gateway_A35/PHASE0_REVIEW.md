# rov_gateway 阶段0 只读审查报告

日期：2026-09-02
范围：Windows↔A35↔M33 网关设计前的只读映射审查。**未修改任何 Windows、M33 或 A35/rov_control 代码。**

依据文档（均已通读）：

- `ROV_M33/AGENTS.md`、`project-docs/PROJECT_STATE.md`、`project-docs/ROV_RUNTIME_HANDOFF.md`、`project-docs/ROV_A35_M33_Control_Protocol_v1.0.md`
- `ROV_M33/A35/rov_control/include/rov/rov_control.hpp`、`rov_types.hpp`、`rov_error.hpp`、`rov_result.hpp`，`src/internal/rpmsg_client.{hpp,cpp}`、`response_parser.cpp`
- `Salacia_Terminal/docs/WINDOWS_A35_INTERFACE.md`（TCP 协议最终权威）
- `Salacia_Terminal/src/communication/WireConstants.h`、`WireCodec.{h,cpp}`、`FunctionRegistry.{h,cpp}`、`TcpClient.{h,cpp}`、`TelemetryPacket.h`
- `Salacia_Terminal/tests/mock_a35.{h,cpp}`、`tests/test_wirecodec.cpp`
- `Salacia_Terminal/config/app_config.ini`、`src/control/ControlViewModel.cpp`、`src/core/SafetyStateModel.h`、`src/MainWindow.cpp`（信号接线）、`src/widgets/CommandPageWidget.cpp`（查询响应显示）
- `ROV_M33/CM33/NonSecure/Drivers/MPU6500/Src/mpu6500.c`（只读核对量程寄存器）

---

## 1. Windows 42 项功能 → A35 处理映射表

帧格式：magic `0x53414C41`、version 1、小端逐字段、CRC16-CCITT-FALSE 覆盖头+载荷。ACK=`0x0101` 回带原 Windows seq，载荷为小端 `u16 errCode`。
优先级：`Estop(0) > Emergency(1) > Stop/Move(2) > 普通(5)`。

“Windows 现状发送”列依据 ControlViewModel / CommandPageWidget / MainWindow / TcpClient 实际调用点（UI 当前不发送的功能也必须实现）。

### 1.1 系统命令（4）

| funcId | 名称 | Windows 现状发送 | A35 处理 | ACK/响应 |
|---|---|---|---|---|
| 0x0001 | ask | 重连后自动发送；指令页按钮 | A35 本地在线应答（RovControl 无 `ask`，M33 未实现该命令，不透传） | ACK ok |
| 0x0002 | ver | 指令页按钮 | A35 本地版本应答；Windows 无此数据帧解码器 | 仅 ACK ok（不发明 payload） |
| 0x0003 | status | 重连后自动发送；指令页按钮 | 刷新权威状态（内部重读 M33 stabilization） | ACK ok + 主动推送 StateEventV2 |
| 0x0004 | help | 指令页按钮 | A35 本地支持的函数清单（日志用） | 仅 ACK ok（不发明 payload） |

### 1.2 安全与 Stop/Move（8）

| funcId | 名称 | Windows 现状发送 | A35 处理（RovControl 调用） | 状态位变化 |
|---|---|---|---|---|
| 0x0010 | stop all | 主页/指令页 | `rov.stop()`（全局锁存+CH10~15 归零；不发舵机） | globalStopped=1 |
| 0x0011 | emergency | 主页（二次确认后） | `rov.stop()`（同上，仅优先级/告警/日志不同） | emergency=1（锁存）, globalStopped=1 |
| 0x0012 | estop | 主页（一键直发） | `rov.stop()`（同上） | estop=1（锁存）, globalStopped=1 |
| 0x0013 | move all | 总使能开关 | `rov.move()`（只解除全局锁存，不恢复旧输出） | globalStopped=0；建议同时清除 estop/emergency 锁存位（决策项 D-02） |
| 0x0014 | stop vertical | 垂直使能开关 | `rov.stopVertical()` | verticalStopped=1 |
| 0x0015 | move vertical | 垂直使能开关 | `rov.moveVertical()` | verticalStopped=0 |
| 0x0016 | stop horizontal | 水平使能开关 | `rov.stopHorizontal()` | horizontalStopped=1 |
| 0x0017 | move horizontal | 水平使能开关 | `rov.moveHorizontal()` | horizontalStopped=0 |

三者 Stop/Emergency/Estop 执行结果完全相同（M33 `stop()` 六路归零），均不携带执行器载荷、不发舵机；区别仅在调度优先级、StateEventV2 位、日志与 AlarmEvent 等级。

### 1.3 模式开关（8）

| funcId | 名称 | Windows 现状发送 | A35 处理 | 状态位 |
|---|---|---|---|---|
| 0x0020 | safe on | Safe 开关 | 若姿态稳定 OFF：先 `rov.enableStabilization()`；成功后置 A35 本地 Safe=ON。任一步失败→错误 ACK，不产生 Safe=ON 且姿态稳定=OFF 组合 | safe=1（且 attitudeStabilization=1） |
| 0x0021 | safe off | Safe 开关 | 只清 A35 本地 Safe（不动姿态稳定） | safe=0 |
| 0x0022 | horizontal on | 姿态稳定开关 | `rov.enableStabilization()`（M33 wire `horizontal on`） | attitudeStabilization=1 |
| 0x0023 | horizontal off | 姿态稳定开关 | Safe=ON 时拒绝（errCode=7）；否则 `rov.disableStabilization()` | attitudeStabilization=0 |
| 0x0024 | vertical synchronization on | 同步开关 | 纯 A35 本地状态（M33 无垂直同步模式；见 §3.3） | verticalSynchronization=1 |
| 0x0025 | vertical synchronization off | 同步开关 | 纯 A35 本地状态 | verticalSynchronization=0 |
| 0x0026 | horizontal synchronization on | 同步开关 | `rov.enableHorizontalSynchronization()`；失败回退 A35 状态+错误 ACK | horizontalSynchronization=1 |
| 0x0027 | horizontal synchronization off | 同步开关 | `rov.disableHorizontalSynchronization()`；失败回退+错误 ACK | horizontalSynchronization=0 |

### 1.4 舵机（5）

| funcId | 名称 | 载荷 | Windows 现状发送 | A35 处理 |
|---|---|---|---|---|
| 0x0030 | set servo | u8 id(0-9)+u16 angle | 控制页滑条（高频） | 校验 id/angle（非法→ACK 2）→ `rov.setServo(id, angle)`。Safe/Stop/同步均不限制舵机 |
| 0x0031 | set servo all | u16 angle | 不发送，须实现 | `rov.setAllServos(angle)` |
| 0x0032 | set servo mid | u8 id(0-9或0xFF) | 不发送，须实现 | 0xFF→`rov.centerAllServos()`；否则 `rov.centerServo(id)` |
| 0x0033 | get servo | u8 id | 指令页 | `rov.getServo(id)` → ACK + 数据帧 0x0033：小端 `i16[1]`（0..180） |
| 0x0034 | get servo all | 空 | 指令页（id=0） | `rov.getAllServos()` → ACK + 数据帧 0x0034：`i16[10]`（CH0..CH9 顺序） |

数据帧说明：Windows `decodeAngleList` 期待 i16 列表（偶数长度≥2、值域 0..180，违例整帧丢弃）；`TcpClient` 不校验数据帧 flags/seq（经 `eventReceived` 上抛指令页显示）。建议 A35 回带原请求 seq、flags=0x02，与事件帧一致。

### 1.5 推进器（7）

| funcId | 名称 | 载荷 | Windows 现状发送 | A35 处理 |
|---|---|---|---|---|
| 0x0040 | set propeller | u8 id(10-15)+i16 pct | 控制页逐通道（同步模式也是逐通道发） | 按当前模式翻译（见 §3.4）；Safe ON 时先施加 A35 限幅 |
| 0x0041 | set propeller all | i16 pct | 不发送，须实现 | 六路同值按 §3.4 规则分别展开 |
| 0x0042 | set propeller stop | u8 id(10-15或0xFF) | 不发送，须实现 | “基准归零”语义，非锁存 Stop（映射决策项 D-05，见 §4-3） |
| 0x0043 | get propeller base | u8 id | 指令页 | `rov.getPropellerBase(id)`（M33 模式不符返回 err safety→ACK 7）→ ACK + 数据帧 `i16[1]`（-100..100） |
| 0x0044 | get propeller real | u8 id | 指令页 | `rov.getPropellerOutput(id)` → ACK + 数据帧 `i16[1]` |
| 0x0050 | base value [弃用] | — | 不发送 | ACK errCode=6（unsupported），记日志 |
| 0x0051 | base value vh | 2×i16（垂直、水平基准） | 姿态稳定基准滑条（主链路） | `setVerticalBase(v)` + `setHorizontalBase(h)`；两者都成功才 ACK ok；部分失败→立即 `rov.stop()` + 错误 ACK + AlarmEvent(level=2) |

### 1.6 传感器与链路（4）

| funcId | 名称 | Windows 现状发送 | A35 处理 |
|---|---|---|---|
| 0x0060 | sensor mpu | 重连后不发；指令页按钮 | `rov.readMpu()` 刷新 MPU 缓存 → ACK ok；随后由 100Hz SensorSummary 携带（不发明数据 payload） |
| 0x0061 | sensor dyp | 指令页按钮 | `rov.readDyp()`（阻塞等待，M33 触发+测量 ~60ms 起）→ ACK ok/err（busy→3，timeout→5，io 映射见 D-06） |
| 0x0062 | sensor all | 重连后自动发送；指令页按钮 | `rov.getSensorSnapshot()` 刷新 DYP 缓存状态 → ACK ok + 推送最新 SensorSummary |
| 0x00F0 | heartbeat | TcpClient 每 1s（flags=0，无 NeedAck） | 静默消费（不 ACK，不回显）；A35 可选用作客户端活性判据（决策项 D-07） |

### 1.7 A35→Windows 事件/遥测（6）

| funcId | 名称 | 触发 | 载荷 |
|---|---|---|---|
| 0x0100 | sensor summary | 连接期间 100Hz（缓存值，慢传感器不逐帧读取） | 定长 45B（见 WINDOWS_A35_INTERFACE §4）；NaN/Inf 不得产生；DYP 无效=-1.0 且清 bit3 |
| 0x0101 | ack | 每个 NeedAck 请求 | u16 errCode；回带原 seq；flags=0（与 mock 一致，Windows 不校验） |
| 0x0102 | state event [legacy] | A35 不主动发送（Windows 仅作兼容回退；决策项 D-08：默认禁发，只走 0x0104） | — |
| 0x0103 | alarm event | BaseValueVH 部分失败、协议异常、传感器失效升级等 | u8 level(0/1/2)+u16 code+u32 boardTimeMs+UTF-8 text（Windows `decodeAlarmEvent` 要求 ≥7B、level≤2） |
| 0x0104 | state event v2 | 连接建立后立即一次；任一位变化后；1Hz 兜底 | u8 version=2 + u16 mask（9 位；未知位 Windows 整帧拒绝） |
| （数据帧） | 0x0033/34/43/44 | 查询命令完成时 | i16[] 列表（见 §1.4/§1.5） |

计数核对：请求 36（系统4+安全3+StopMove5+模式8+舵机5+推进器5+基准2+传感器3+心跳1）+ A35→Win 5 = **41**，与 Windows FunctionRegistry 实际条目数一致。勘误：接口文档 §3 标称"42 条"为文档误差（阶段1按源码清点核实，见 DECISIONS.md D-15）；网关以 41 条全表为准。

### 1.8 errCode 与 RovError 映射（拟）

| errCode | 含义 | RovError 来源 |
|---|---|---|
| 0 | ok | None |
| 1 | bad_cmd | BadCommand；未知 funcId（NeedAck 时） |
| 2 | bad_arg | BadArgument；载荷长度/ID/值域校验失败（Windows 已前置校验，A35 仍须独立校验） |
| 3 | busy | Busy（如 DYP 测量进行中） |
| 4 | not_ready | NotReady；M33/RPMsg 未连接（Disconnected/TransportIo 时 A35 未就绪） |
| 5 | timeout | Timeout；Io/ProtocolError 的建议归并（决策项 D-06） |
| 6 | unsupported | Unsupported（0x0050 弃用项等） |
| 7 | safety | Safety（M33 拒绝、Safe 联动拒绝、姿态稳定下 individual 拒绝等） |

---

## 2. A35→RovControl 映射表（反向视角）

| RovControl API | 网关用途 | 备注 |
|---|---|---|
| open/close/isOpen | 启动时独占 `/dev/ttyRPMSG0` | 前置：remoteproc0 running、`rov_self_test` 已退出 |
| setServo / setAllServos / centerServo / centerAllServos | 0x0030~0x0032 | angle 为 u8 0..180（wire 是 u16，A35 收窄前校验） |
| getServo / getAllServos | 0x0033/34 数据帧 | 返回软件目标角，非机械反馈 |
| setVerticalBase | 姿态稳定 ON 时的 CH10~13 组值（0x0040 垂直/0x0051 v 分量/0x0041 展开） | |
| setVerticalPropeller | 姿态稳定 OFF 时的 CH10~13 单路 | 稳定 ON 时 M33 拒绝（err safety）——A35 不得在此模式下透传 |
| setHorizontalBase | 水平同步 ON 时的 CH14/15 组值 | |
| setHorizontalPropeller | 水平同步 OFF 时的单路 | 同步 ON 时 M33 拒绝 |
| getPropellerBase / getPropellerOutput | 0x0043/44 数据帧 | base 查询受 M33 模式限制（err safety 可能） |
| getAllPropellerBases / getAllPropellerOutputs | 状态对齐与内部一致性 | `all base` 需两种 base 模式均启用 |
| enableStabilization / disableStabilization | 0x0022/23、SafeOn 联动第一步 | M33 wire 名为 `horizontal on/off`，实为 Roll/Pitch 姿态稳定 |
| enableHorizontalSynchronization / disableHorizontalSynchronization | 0x0026/27 | 只作用于 CH14/15 |
| readMpu | 0x0060 + 50~100Hz 周期采集 | 原始 int16，A35 转换单位（§3.6） |
| readDyp | 0x0061 + 5~10Hz 或查询触发 | 阻塞式（异步 pending，M33 完成后回复）；busy/timeout 可能 |
| getSensorSnapshot | 0x0062；DYP 缓存状态判定 | 含 dypState/busy/valid/age |
| getAttitude / getStabilization | status 命令、启动状态对齐 | StabilizationStatus 含 `he/gs/vs/hs` 四个模式/锁存位 |
| stop / move / stopVertical / moveVertical / stopHorizontal / moveHorizontal | 0x0010~0x0017、Emergency/Estop、BaseValueVH 部分失败回退、退出兜底 | stop 归零六路推进器且锁存；move 只解锁不恢复输出 |

**RovControl 刻意不提供**（M33 未实现，网关必须本地实现，不得伪造 M33 能力）：`ask`、`ver`、`status`、`help`、`emergency`、`safe`、`event`。

**RpmsgClient 并发能力（已核实源码）**：`request()` 线程安全——写互斥（`writeMutex_` 串行化写入）、pending map 按 SEQ 分发、独立 reader 线程、超时与迟到响应隔离（quarantine 5s）。因此“DYP 等待期间并发 stop()”在客户端层面可行；时序保证需在阶段2用 Fake/实测验证（VibePrompt §8 的四项测试保留）。

---

## 3. A35 本地实现项（不依赖 RovControl 的部分）

### 3.1 TCP 服务端
- 监听 :7000（可配置）；小端编解码、CRC16-CCITT-FALSE、帧累计器（半包/粘包/坏 magic/坏 CRC 重同步、超长与溢出断线）——语义对齐 Windows `FrameAccumulator`（含 `len>max_payload` 清缓存判失步）。
- SIGPIPE 忽略；单 accept 循环；客户端异常退出不致服务退出。
- 单控制权策略（D-04）：默认建议“新连接接管、旧连接断开”（与 Mock 行为一致），可配置“拒绝新连接”。

### 3.2 权威状态机（StateEventV2 九位）
- bit0 safe、bit1 attitudeStabilization、bit2 globalStopped、bit3 verticalStopped、bit4 horizontalStopped、bit5 verticalSynchronization、bit6 horizontalSynchronization、bit7 estop、bit8 emergency。
- 推送时机：连接建立后立即、任一位变化后、1Hz 兜底。
- 不变量：safe=1 ⇒ attitudeStabilization=1（A35 保证不产生该非法组合）；ACK 成功与权威状态一致。
- 启动对齐（D-01）：`getStabilization()` 读 `he/gs/vs/hs` 对齐姿态稳定与三个 stop 锁存；M33 synchronization 状态**不可直接查询**（协议无该查询、telemetry 不含），需 probe（`getPropellerBase(14)` 返回 err safety ⇒ 同步 OFF）或启动时强制 `disableHorizontalSynchronization()` 建立已知态。协议文档记载 M33 启动默认 `horizontal on`（稳定 ON），不能假设为关闭。

### 3.3 垂直同步 = 纯 A35 状态
Windows bit5 verticalSynchronization 影响“终端布局与提交方式”，M33 无对应模式。A35 翻译规则（VibePrompt §6.3 建议，待 Fake 测试固化）：
- 姿态稳定 ON：垂直组值 → `setVerticalBase()`（与垂直同步位无关）。
- 姿态稳定 OFF 且垂直同步 ON：同组同值请求展开为 CH10~13 四路相同 individual。
- 姿态稳定 OFF 且垂直同步 OFF：逐路 individual 透传。
- 水平同步 ON：CH14/15 → `setHorizontalBase()`；OFF：individual。
- 同步模式切换失败：回退 A35 状态位 + 错误 ACK。

### 3.4 set propeller 翻译矩阵（0x0040/0x0041）

| Windows 通道 | 姿态稳定 | 水平同步 | M33 调用 |
|---|---|---|---|
| CH10~13 单路 | OFF | — | `setVerticalPropeller(id, v)` |
| CH10~13（任一/同值组） | ON | — | `setVerticalBase(v)`（四路归基准） |
| CH14/15 单路 | — | OFF | `setHorizontalPropeller(id, v)` |
| CH14/15（任一/同值组） | — | ON | `setHorizontalBase(v)` |

### 3.5 Safe 限幅（A35 本地）
- Safe=ON 期间对发往 M33 的推进器值施加限幅（推进器基准值域内保守上限，默认建议 ±30%，**未经用户确认不得用于真实推进器**——决策项 D-09，须写入 DECISIONS）。
- Safe 不控制舵机、不等同 Stop、不自动归零。

### 3.6 传感器链
- **INA226**（A35 本地，hwmon）：见 §5。
- **DHT11**（A35 本地，待实机确认 ABI）：见 §5。
- **MPU/DYP**（经 RovControl）：MPU 50~100Hz 周期读缓存；DYP 5~10Hz 或查询触发。
- 单位换算（已核对 M33 源码 `mpu6500.c`：GYRO_CONFIG=0x00、ACCEL_CONFIG=0x00 ⇒ ±250dps/131 LSB、±2g/16384 LSB，与姿态估计器常量一致）：
  - `accelMps2 = raw / 16384.0 × 9.80665`
  - `gyroRadS = raw / 131.0 × π / 180.0`
- SensorSummary 45B @100Hz：值+时间戳+TTL+错误态独立维护；单传感器失效只清自身 valid 位；不发送 NaN/Inf；`boardTimeMs` = A35 单调钟低 32 位。

### 3.7 调度与线程解耦
网络接收/帧解析、紧急控制（Estop/Emergency 直通道）、普通控制、M33 传感器采集、A35 本地传感器采集、100Hz 汇总发送、状态/告警发布各自独立；紧急命令不因普通队列满而丢失、不被 DYP 阻塞（Estop 开始执行 ≤100ms 量级目标，阶段2/4 验证）。

---

## 4. 已确认 / 未确认的传感器用户空间接口

### 4.1 INA226（**已实机确认**，PROJECT_STATE 2026-08-27 条目）
- 总线：I2C3 = Linux `i2c-0`（`0x40140000`），地址 0x40（7-bit），板上另有 PCF8563 @0x51。
- 驱动：内核自带 `ina2xx.ko` 支持 `ina226`；实测 `echo ina226 0x40 > /sys/bus/i2c/devices/i2c-0/new_device` 绑定成功，`/sys/class/hwmon/hwmon1/name = ina226`，Rshunt=10000 µΩ。
- 读数：`in1_input` 单位 **mV**（实测 4955 mV ≈ 4.955 V）；换算 V = mV/1000。
- 网关实现要求：按 `name=="ina226"` 扫描 `/sys/class/hwmon/hwmon*/` 发现（禁硬编码 hwmon 编号）；支持配置覆盖路径；范围检查 + 失败处理。
- 限制：运行时 `new_device` 是临时手段；正式 DT 节点尚未建立。网关部署清单须包含“确认 ina226 已绑定”检查，缺失时只清 valid bit2 并告警（不自动改系统状态，除非用户授权）。

### 4.2 DHT11（**未确认**，阶段3实机第一项）
- 现有证据仅：“优先使用板卡已有 Linux dht11 驱动，待新硬件到货完成最终实机验证”（协议文档 §10/§18）。板卡是否已含 DT 绑定、挂载 GPIO、设备编号均未验证。
- 上游内核 `dht11` 驱动为 IIO 框架（预期 `/sys/bus/iio/devices/iio:deviceX`，`in_temp_input` 毫摄氏度、`in_humidityrelative_input` 毫 %RH），但**该 ABI 归属必须实机确认后才能编码**。
- 网关要求：发现式/可配置路径、超时与陈旧判定、范围检查（DHT11 0~50°C / 20~95%RH）、失败不阻塞 TCP 线程、暂时消失后重试。
- 结论：列入阶段3首要实机任务与风险项。

### 4.3 MPU（M33 侧，已确认量程）与 DYP（M33 侧，语义限制）
- MPU：M33 固件 ±2g/±250dps（源码核对），换算系数 §3.6；若 M33 量程变更须重新推导。
- DYP：`distanceMm` 原样保留 M33 值；**65533 仅证明 UART 帧合法，不是确认的 sentinel**。A35 有效位判定建议基于 RovControl 结果（成功且值在物理合理区间→bit3 置位；timeout/io_error/busy/超区间→清 bit3、distMm=-1.0），区间阈值可配置（Windows 侧显示用 20..8000mm 可作参考初值）——决策项 D-10。

---

## 5. 仍有歧义/待决策项清单

| 编号 | 问题 | 现状与建议 |
|---|---|---|
| D-01 | M33 synchronization 初始状态不可查询 | 启动 probe（`getPropellerBase(14)` 的 err safety 推断）或强制 disable 建立已知态；写入设计并 Fake 测试 |
| D-02 | estop/emergency 位清除时机 | Windows 无清除命令；建议 MoveAll（0x0013）成功后一并清除两锁存位并推 StateEventV2；需写入 DECISIONS |
| D-03 | 数据帧 flags/seq 约定 | Windows 不校验；建议：数据帧回带原 seq、flags=0x02；ACK flags=0（与 mock 一致） |
| D-04 | 第二客户端策略 | 默认“新连接接管”（mock 行为），可配置“拒绝”；两种策略均需测试 |
| D-05 | 0x0042 set propeller stop 映射 | 单路：模式允许时 individual 置 0（稳定 ON 时 CH10~13 单路会被拒→改走 base 0？）；0xFF：垂直/水平各自按当前模式置 base 0 或展开 individual 0。须在阶段2定稿并测试（不使用锁存 stop，避免与 StopAll 语义混淆） |
| D-06 | RovError::Io / ProtocolError → errCode | 建议归并 5（timeout）+ AlarmEvent/日志区分真因；errCode 表只有 8 值，无 io 专用码 |
| D-07 | 心跳与断连策略 | A35 静默消费 0x00F0；是否以心跳超时+`stop_on_disconnect` 判定客户端死亡（默认建议启用，真实推进器测试前必须用户确认并记录） |
| D-08 | 是否发送 legacy 0x0102 | 建议不发送，仅 0x0104 |
| D-09 | Safe 限幅默认参数 | 保守默认 ±30% 占位，待边界确认；真实推进器前用户确认 |
| D-10 | DYP 有效性判定区间 | 建议 20..8000mm 可配置；65533 不定义为 sentinel（遵守 M33 文档） |
| D-11 | ask 语义 | 仅证明 A35 在线（TCP 已建连本身即证明）；不透传 M33（无 API）。可选：内部顺带校验 RovControl isOpen 用于 status，不影响 ask 的 ACK ok |
| D-12 | CRC 黄金向量 | Windows 测试无显式向量；采用公开向量 `"123456789"` → `0x29B1`（CCITT-FALSE）+ 与 Windows `telemetryCrc16` 实现对拍的本地向量 |
| D-13 | `get servo` 数据帧为空/越界值处理 | `decodeAngleList` 值域 0..180；M33 返回值天然合法；若非法值出现，A35 记日志并不发数据帧（避免 Windows 丢弃告警噪音）——实现细节，阶段1定 |
| D-14 | Windows 未定义数据 payload 的查询（ver/status/help/sensor×3） | 按 VibePrompt：仅 ACK，status 附带 StateEventV2，sensor 类附带 SensorSummary；不发明 payload（已定，列出备查） |

---

## 6. 拟新增/修改文件清单（阶段1起，全部位于 Gateway_A35）

**不修改**：`ROV_M33/**`（含 `A35/rov_control` 源头）、`Salacia_Terminal/**`、M33 wire 协议、OpenAMP/remoteproc、设备树。

```text
Gateway_A35/
├── PHASE0_REVIEW.md                 # 本报告
├── CMakeLists.txt                   # C++17、Release、板端原生构建（CTest）
├── README.md                        # 编译/启动/停止/排障（阶段5完善）
├── config/rov_gateway.ini           # 示例配置（TCP/RPMsg/采样率/Safe/单客户端/日志）
├── deploy/rov_gateway.service.in    # systemd unit 模板（After= 实机核实后定）
├── vendor/rov_control/              # 从 ROV_M33/A35/rov_control 复制（不改动，保持 API v1）
│   ├── include/rov/*.hpp            # rov_control/rov_error/rov_result/rov_types
│   └── src/**                       # rov_control.cpp + internal/{rpmsg_client,response_parser,sequence_allocator,transport,posix_tty_transport}
├── src/
│   ├── main.cpp                     # 信号处理、启动对齐、优雅退出（best-effort stop）
│   ├── wire/                        # crc16、wire_codec、frame_accumulator、function_registry(42项)
│   ├── net/                         # tcp_server（单控制权、心跳、断线清理）
│   ├── core/                        # gateway_state(9位权威)、dispatcher、priority_queue、
│   │                                # safe_policy、sync_translator、base_value_vh
│   ├── sensors/                     # ina226_reader、dht11_reader、m33_sensor_reader、
│   │                                # sensor_cache(TTL/validMask)、summary_builder(45B@100Hz)
│   └── util/                        # config(ini)、logger(journald 友好)、clock
└── tests/                           # Fake RovControl / Fake SensorReader + VibePrompt §11 全部主机测试
```

（VibePrompt 阶段1原文写 `A35/rov_gateway`，本方案按用户指定统一落在 `Gateway_A35` 根目录，内部不再嵌套同名层。）

---

## 7. 完成报告要素（按 VibePrompt §13）

- 新增/修改文件：仅本报告 `Gateway_A35/PHASE0_REVIEW.md`；未修改任何工程代码。
- 构建结果：不适用（只读阶段）。
- 测试数量：0（无代码）；审查覆盖 42/42 注册表项、Windows 全部实际发送路径、RovControl 全部公开 API、RpmsgClient 并发路径。
- 实机验证证据：无新实机操作；引用的实机事实全部来自 PROJECT_STATE 已验证条目（INA226 hwmon、RovControl API v1、rov_self_test、DYP UART、MPU 量程源码核对）。
- 已确认事实：见 §1~§4（帧格式/42 项 funcId/错误码/状态位/锁存语义/数据帧 i16[] 解码器/RpmsgClient 线程安全/INA226 ABI/MPU 量程）。
- 尚存不确定项：D-01~D-14（§5），其中 DHT11 ABI（§4.2）与 M33 同步初始态（D-01）影响最大。
- 安全风险：Safe 限幅参数未定（D-09）；`stop_on_disconnect` 默认值需用户确认（D-07）；estop/emergency 清除语义（D-02）须在实现前定稿；第二客户端接管瞬间旧连接的推进器目标处理（D-04）。
- PROJECT_STATE/DECISIONS/TROUBLESHOOTING：阶段0未更新 ROV_M33 文档（无代码、无实机事实）；D-02/D-04/D-07/D-09 建议在阶段2实现前固化进 `Gateway_A35/DECISIONS.md`。

**阶段0到此为止。等待批准后进入阶段1（网关骨架与协议层）。**
