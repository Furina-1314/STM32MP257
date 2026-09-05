# rov_gateway 决策记录（DECISIONS）

本文件记录经用户批复的设计决策与阶段0审查（PHASE0_REVIEW.md）中 D-01~D-14 的处置。
日期：2026-09-02（阶段0批复时建立）。

---

## A. 用户批复指令（2026-09-02，原文编号 U-00~U-05）

### U-00 板上自启动与系统编排
未来 A 核程序需在板上自启动，并负责调用 CamStream 推流和启动 M 核程序。
- 归属阶段5（OpenSTLinux 部署）。
- `rov_gateway.service`：`ExecStartPre` 依次完成 `fw_cortex_m33.sh start` → 轮询 `/dev/ttyRPMSG0` → 运行 `rov_self_test` 并等待退出（exit 0 方可继续）；`After=`/`Requires=` 必须先检查板上实际存在的 systemd 服务再定，不凭空引用 remoteproc unit 名。
- CamStream 由网关负责拉起并监管（子进程管理或配套 unit，阶段5定稿）；网关不得干扰 CamStream 的 UDP 5000 视频。
- 注意与 M33 幂等启动的配合：`fw_cortex_m33.sh start` 已在 running 时不应反复 stop/start（交接手册 SOP）。

### U-01 垂直/水平同步的实现归属
- **垂直同步**：由 A35 组装——一条 Windows 组语义展开为多条 M33 `setVerticalPropeller()` individual 命令（CH10~13 同值）；A35 不向 M33 发送任何"垂直同步"命令（M33 无此命令）。
- **水平同步**：调用现有接口 `enableHorizontalSynchronization()/disableHorizontalSynchronization()`（M33 `synchronization on/off`，仅作用 CH14/15）。
- 翻译矩阵见 PHASE0_REVIEW §3.3/§3.4，以阶段2 Fake 测试固化。

### U-02 网关启动同步对齐
A 核程序启动时首先对齐两个同步开关，并向 Windows 终端报告两个 synchronization on：
- 水平：`rov.enableHorizontalSynchronization()`（真实 M33 命令；M33 启动默认已是 1，该 set 幂等，发送一次建立已知态）。
- 垂直：设置 A35 本地 `verticalSynchronization=true`（无对应 M33 命令，依据 U-01 由 A35 组装）。
- 对 Windows 的"响应"＝连接建立后立即推送的 StateEventV2 中 bit5、bit6 均为 1（网关启动早于 Windows 连接，无法在启动瞬间"返回"，以连接后首次全量推送兑现）。

### U-03 网关启动姿态稳定对齐
M33 默认启动 horizontal on（源码 `PropellerService_Init`: `horizontal_enabled=1U`）；安全起见网关启动时仍显式发送一次 `rov.enableStabilization()`（幂等），并在权威状态中置 `attitudeStabilization=true`，随连接后首次 StateEventV2 报告给 Windows。

启动对齐完整顺序（阶段2实现）：
1. 前置检查（remoteproc running、`/dev/ttyRPMSG0`、self-test 已退出）；
2. `RovControl::open()`；
3. `getStabilization()` 读取 he/gs/vs/hs 对齐姿态稳定与三个 stop 锁存（self-test 后 gs 预期为 1）；
4. `enableStabilization()`（一次）→ `enableHorizontalSynchronization()`（一次）→ A35 本地 verticalSync=true；
5. safe/estop/emergency 初始 false。
任一步失败：退避重试 + 告警日志；权威状态未建立前不向 Windows 确认模式成功。

### U-04 数据帧格式正确性归 A35
A 核负责全部 A35→Windows 数据帧的格式正确：45B SensorSummary 定长与字段偏移、小端逐字段、StateEventV2 version=2 且只含 9 个已定义位、ACK 回带 seq、i16 列表值域（角度 0..180 / 百分比 ±100）、告警事件 level≤2、不产生 NaN/Inf（构造端即消毒）。编码层为此提供带校验的 builder（阶段1交付）。

### U-05 stop 在 horizontal on 下的行为——现有接口核查结论
**已核查 M33 源码（阶段0批复后的只读复查），功能不缺失，无需补偿逻辑**：
- `PropellerService_StopAll()`（wire `stop`）无任何模式门控：不检查 `horizontal_enabled`/`synchronization_enabled`，直接置 `global_stopped=1` 并将 CH10~15 写 0。
- 姿态 PID 的写入路径 `PropellerService_ApplyVerticalControl()` 在 `VerticalBlocked()`（global/vertical stop 锁存）时返回 SAFETY 拒绝写入——stop 后 PID 不会重新写非零输出。
- 结论：网关 Stop/Estop/Emergency 直接调用 `rov.stop()`，**不需要**"先 horizontal off 再置零"。
- 证据等级：源码静态核查（`propeller_service.c` L369-385、L449-467）；阶段4实机用 `--thrusters` 类台架复核一次。
- 附带核实：`PropellerService_Init()` 初始 `synchronization_enabled=1U`、三锁存=0（即 M33 冷启动为"双模式 ON + move 态"，self-test 后才置 global stop）。

---

## B. 阶段0 D-01~D-14 处置（"其他按你的建议执行"批复）

| 编号 | 处置 |
|---|---|
| D-01 | 按 U-02/U-03 执行：启动显式发送同步/稳定 on（幂等）+ `getStabilization()` 读锁存；放弃 probe 方案 |
| D-02 | 采纳：MoveAll(0x0013) 成功后一并清除 estop/emergency 锁存位并推 StateEventV2 |
| D-03 | 采纳：数据帧回带原 seq、flags=0x02；ACK flags=0 |
| D-04 | 采纳：默认"新连接接管"（断开旧连接），配置项可改为"拒绝新连接"；两策略均测试 |
| D-05 | `set propeller stop`(0x0042)= 基准归零非锁存：单路按当前模式（稳定 ON→CH10-13 走 base 0；同步 ON→CH14/15 走 base 0；否则 individual 0）；0xFF=垂直+水平各自按模式归零。阶段2 Fake 测试固化 |
| D-06 | 采纳：RovError::Io/ProtocolError/TransportIo→errCode 5，DisConnected/NotReady→4，SequenceExhausted→3；真因入日志/AlarmEvent |
| D-07 | 采纳：心跳静默消费；A35 侧心跳超时+`stop_on_disconnect` 默认启用，**真实推进器测试前必须用户确认**并回写本文件 |
| D-08 | 采纳：不发送 legacy 0x0102，仅 0x0104 |
| D-09 | Safe 限幅默认 ±30% 占位（可配置），真实推进器测试前必须用户确认 |
| D-10 | DYP 有效性区间 20..8000mm 可配置；65533 不定义为 sentinel（透传但可按区间判无效） |
| D-11 | ask 仅证明 A35 在线，ACK ok；不透传 M33 |
| D-12 | CRC 黄金向量：`"123456789"`→0x29B1（公开标准值）、空输入→0xFFFF，另加独立查表实现交叉验证 |
| D-13 | 非法 M33 查询值：记日志、不发数据帧（避免 Windows 侧丢帧告警噪音） |
| D-14 | ver/status/help/sensor×3 仅 ACK；status 附 StateEventV2；sensor 类附 SensorSummary |
| D-15 | **注册表条数勘误**：接口文档 §3 称"42 条"，但 Windows `FunctionRegistry` 实际为 **41 条**（请求 36 + A35→Windows 5；已按源码逐项清点核实，Windows 自身测试仅要求 ≥26）。网关以代码事实为准实现 41 条全表；PHASE0_REVIEW §1.7 的"42"计数相应更正。 |

### 阶段2新增决策（2026-09-02）

| 编号 | 决策 |
|---|---|
| D-16 | **出站帧顺序**：同一次命令处理中先 ACK 后 StateEventV2；数据帧（get servo/propeller 响应）在 ACK 之后。Windows 对"事件先于 ACK"与"ACK 先于事件"均兼容（文档 §7），选定后者以减少 lateAck 噪音。 |
| D-17 | **心跳超时**：A35 侧默认 5000ms 判客户端失联（可配置 0=禁用）。依赖 Windows 端心跳默认开启（1s）；部署配置两侧须一致，否则应禁用 A35 侧判定。TCP 层断开（RST/FIN）始终立即触发断线路径。 |
| D-18 | **组命令部分失败**：`set propeller all`(0x0041)/`set propeller stop`(0x0042) 任一 M33 调用失败即中止后续并按首个错误 ACK；不自动全局 Stop（强制 Stop+Alarm 仅限 0x0051 BaseValueVH，见 VibePrompt §6.4）。 |
| D-19 | **Safe 限幅生效条件勘误**：限幅仅在 Safe=ON 时生效（阶段1草案曾无条件限幅，实现阶段纠正）。Safe=OFF 时推进器值原样透传；协议 §6.1"在姿态稳定基础上执行边界检查/限幅"为 Safe 模式内行为。 |
| D-20 | **组展开的 base 合并**：base 模式下组语义每组只发一次 base 调用（垂直组→1×setVerticalBase，水平组→1×setHorizontalBase）；individual 模式才逐通道展开（垂直4路/水平2路）。单通道请求（0x0040）在 base 模式下映射为该组 base。 |

### 阶段3新增决策（2026-09-03，依据 Drivers.md 与快速体验手册 PDF 4.25 节）

| 编号 | 决策 |
|---|---|
| D-21 | **DHT11 ABI**：正点原子出厂固件使用自定义 `drivers/char/dht11`，用户空间接口为 `/sys/class/misc/dht11/value`（4位数字=前2湿度+后2温度；PDF 实例 9925=99%RH/25℃）。网关默认 `alientek_misc`，`auto` 模式先 misc 后标准 IIO；标准 IIO 后端按 `/sys/bus/iio/devices/*/name=="dht11"` 发现（不写死设备号，毫单位换算 /1000）。模块加载（rmmod ds18b20 / modprobe dht11）属部署步骤，程序不在采样时执行 modprobe/cat——按 Drivers.md §四。 |
| D-22 | **DHT11 三位数据策略**：默认拒绝猜测（AmbiguousFormat，保留原始值）；可配置手册兼容策略（前2位湿度+剩余1位温度），启用时 `inferred=true` 并记录警告。两位数据恒为歧义。周期下限 1s（服务端钳位）。 |
| D-23 | **INA226 读取**：按 `/sys/class/hwmon/hwmon*/name=="ina226"` 发现（禁硬编码编号，节点消失/重编号自动重扫）；属性以有符号64位读取再换算（in1 mV→V、curr1 mA→A 允许负值、power1 µW→W、shunt_resistor µΩ）；in1_input 必需，其余可选缺失只置标志；母线电压范围检查默认 0~36V 可配置。 |
| D-24 | **传感器查询的刷新语义**：`sensor mpu`/`sensor all` 在 TCP 线程内联快速 MPU 读（单次 RPMsg 往返）；`sensor dyp` **不内联执行**阻塞测量（M33 响应窗 ~60ms+busy），DYP 线程按 5Hz 连续采集，查询返回的缓存至多一个周期旧（≤200ms），刷新值经 100Hz 流自然到达。三者成功 ACK 后均推送一帧最新 SensorSummary（D-14）。 |
| D-25 | **出站队列有界**（512 帧，丢最旧）：防止无客户端连接时 100Hz 流无限增长；ACK/事件/遥测同队列， newest-wins 对遥测无损。 |
| D-26 | **陈旧判定**：各传感器独立 `staleFactor(3) × periodMs` 窗口；失效只清自身 validMask 位；DYP 无效恒为 -1.0mm 哨兵；数值不为 0 伪装有效（Drivers.md §六）。 |

### 阶段4实机确认（2026-09-03，ATK-DLMP257B / 192.168.1.120）

| 编号 | 事实 |
|---|---|
| R-01 | **启动对齐实机验证**：M33 冷启动 `he=1`（稳定 ON，实机复现源码结论 U-03）；self-test 后 `gs=1` 锁存；网关对齐后首帧 StateEventV2 mask=0x0066（stab+vsync+hsync+gstop），与 U-02/U-03 设计一致。 |
| R-02 | **100Hz 汇总流实机验证**：2 秒收 201 帧（≈100Hz）、45B 载荷；validMask=0x03——DHT11 bit0 置位（实机读数 `4930`=49%RH/30℃，正点原子 misc ABI 实测有效，D-21 确认）；MPU bit1 置位；INA226 bit2 清除（板上 hwmon 仅有 `pvt`，无 DT 节点，诊断信息正确）；DYP bit3 清除（实机 65533 超出 [20,8000]mm 窗口被 D-10 策略判无效）。 |
| R-03 | **Estop 延迟实机验证**：DYP 5Hz + MPU 50Hz 轮询同时进行时，20 轮 Estop ACK 往返 min=5.1ms avg=6.4ms max=7.2ms——远低于 100ms 目标（RpmsgClient 并发路径实机成立，阶段0源码结论获验证）。 |
| R-04 | **优雅退出实机验证**：SIGTERM 后进程干净退出，日志完整（open→align→listen→shutdown→stopped）；退出后 smoke 确认 M33 `gs=1` 锁存保持、`he=1` 不受影响（U-05 补充实机证据：self-test 与 estop 均在稳定 ON 下成功 stop）。 |
| R-05 | **协议字节级端到端**：Windows 主机（192.168.1.100）直连板卡 7000 端口收帧，首字节 `41 4C 41 53 | 01 | 0100 | 0000 | 02 | 002D` 逐字段与协议 §1 一致（magic/version/funcId=SensorSummary/seq/flags=Event/len=45）。 |
| D-27 | 阶段4实现勘误：`std::atomic<int> failures_[4]` 未初始化（C++17 atomic 默认构造不置零）导致板上失败计数从垃圾值起步——已加 `= {}` 值初始化并加计数饱和（>1e6 停止累加与日志）。教训记入：跨平台未显式初始化的原子成员是实机才暴露的典型缺陷。 |

### 阶段5部署决策（2026-09-03）

| 编号 | 决策 |
|---|---|
| D-28 | **传感器查询去重**：`sensor mpu`/`sensor all` 的 ACK 校验读之后，bridge 不再发起第二次读（实测重复 MPU 查询会加剧 M33 侧迟滞，见 T-01）；缓存新鲜度完全由周期轮询线程负责。 |
| D-29 | **轻量轮询部署默认**：`mpu_period_ms=1000`、`dyp_period_ms=5000`（T-01 缓解；轮询关闭时全部探针 PASS 证明毒源在周期轮询的长期运行效应）。1Hz MPU 数据使 Windows 三维姿态刷新变粗，属已知折衷，待 M33 侧根因闭环后回调 50Hz。 |
| D-30 | **systemd 编排（U-00 落地）**：`rov_gateway.service`（enabled）：`After=st-m33firmware-load.service`；ExecStartPre 依次为 `rov_m33_preflight.sh`（仅当 remoteproc 非 running 或固件名不符时才重载固件——网关重启**不得**重启 M33 以保锁存）→ `wait_rpmsg.sh`（15s 限时）→ `rov_self_test`（exit 0 方可继续）；`Restart=on-failure` + StartLimit 5次/300s；重启后网关只做状态镜像、绝不自动 move。`rov_camstream.service`（installed/disabled）：gst-launch 管线与网关独立生命周期，目标地址从 `/etc/rov_gateway.ini [camstream]` 读取（替换原脚本中失效的硬编码地址），需视频时 `systemctl enable --now rov_camstream`。 |
| D-31 | **配置文件**：`/etc/rov_gateway.ini`（示例在 `config/rov_gateway.ini`），缺失时网关以内置默认运行；[tcp]/[rov]/[gateway]/[sensors]/[log] 映射三套 Config，[camstream] 供视频 unit 使用。 |

### 阶段5实机确认（2026-09-03）

| 编号 | 事实 |
|---|---|
| R-06 | **U-00 开机自启动全链路**：整机 reboot 后 journal 完整记录 vendor 默认固件加载失败（offline）→ preflight 换载 ROV 固件并启动 M33 → `/dev/ttyRPMSG0` 出现 → self-test `PASS WITH WARNINGS`(exit 0) → 网关 active，Windows 主机可直连 7000。 |
| R-07 | **服务生命周期**：`systemctl restart` 不重启 M33（dmesg m33-up 计数不变，preflight 条件跳过）；`kill -9` 后 6s 内 Restart=on-failure 复活（新 pid）；`systemctl stop` 保持停止且无自动重启；优雅退出后 M33 stop 锁存保持。 |
| R-08 | **隔离实验**：轮询关闭时探针全 PASS（对齐位全对/全部 M33 查询正常/Estop 5-7ms）；带轮询长跑后数据类查询超时+ENOMEM（T-01）。 |

### 阶段6实机确认（2026-09-03，Windows↔A35↔M33全链路）

| 编号 | 事实 |
|---|---|
| R-09 | **Windows行为一致性（LAN实测，48项ALL PASS）**：连接推送/重连自动查询/101Hz汇总/Safe联动（含h_off拒绝7）/推进器翻译全路径（individual/base/同步切换/零值/BaseValueVH）/Stop-Move三级锁存（含锁存下set拒绝7）/舵机set-get-mid（数据帧90）/值域独立校验（181/101/-101→2）/弃用项与未知funcId→6/心跳静默/垃圾重同步/estop LAN RTT 4.2ms/接管策略。测试脚本：`scripts/phase6_conformance.py`。 |
| R-10 | **真实Salacia_Terminal对接**：发现 out/deploy 部署件为8月26日旧构建（仍走SSH、无v2协议）——替换为 out/build/release 的9月2日当前构建后启动：配置正确加载（板端192.168.1.120）、`TCP：已连接 192.168.1.120:7000`、AI/遥测就绪；板端 tcpdump 确认持续向终端送达 59 字节包（45B SensorSummary+14B帧头=100Hz汇总流）。视频未推流（rov_camstream 默认未启用，终端日志给出对应提示属预期）。 |

---

## C. 后续须回填本文件的项

- [ ] D-07/D-09 真实推进器测试前的用户确认记录
- [x] 阶段4：U-05 结论的实机复核结果（R-04：稳定 ON 下 stop 直接成功）
- [x] 阶段3：DHT11 实机 ABI 确认结果（R-02：alientek misc，4930=49%RH/30℃）
- [x] 阶段5：U-00 的 systemd 最终方案（D-30：st-m33firmware-load + preflight 条件重载 + camstream 独立 unit）
- [ ] T-01 M33 侧根因闭环后的轮询频率回调与看门狗方案
- [ ] 阶段5：INA226 设备树节点方案（当前板上无节点，R-02）

### M33 数据链路看门狗（2026-09-04，用户批准）

| 编号 | 决策 |
|---|---|
| U-06 | **看门狗批准**：用户确认实施"M33 楔死自动恢复"（台架阶段；下水前须重评）。触发条件：MPU 轮询持续失败超 `mpu_watchdog_timeout_ms`（默认 60s，0=禁用；轮询周期 >= timeout/2 自动禁用以防慢轮询误触发）。 |
| D-29 | **看门狗实现为"退出码 + systemd 自愈链"**：网关主循环 5s 检查 `SensorService::watchdogState()`（纯函数 `mpuWatchdogFires` 可单测），触发时记录 ERROR 并走正常关停路径后以退出码 90 退出；systemd `Restart=on-failure` 拉起重启链，`rov_m33_gate.sh`（新 ExecStartPre）在自检失败时一次性重启 M33 并经自检重落全局 stop 锁存，二次失败交由 StartLimit 熔断。不在网关进程内直接重启 M33（保持 RPMsg 单一 owner 语义）。 |
| R-09 | **实机演练证据（2026-09-04 12:30）**：`fw_cortex_m33.sh stop` 制造数据链路死亡 → journal 记录 `mpu failure xN: rov error 10`（失败日志同期上线）→ 60s 后 `M33 data-path watchdog fired (mpu dead 61102 ms)` → 进程退出 → systemd 重启 → preflight 重载固件 → m33_gate 自检 PASS 且 stop 锁存保持 → 探针 0 失败、validMask=0x07。闭环自愈验证通过（本次触发值为默认 60s：演练时 ini 键误置于 [sensors] 段未被读取，已修正至 [gateway] 段）。 |
