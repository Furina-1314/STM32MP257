# Windows↔A35 TCP 接口说明（v2 最终版）

> 本文档描述 Salacia_Terminal（Windows 侧）实现的 TCP 控制通道与权威状态模型。
> **状态：协议最终版（2026-09-02 定稿，以 Windows 侧现状实现为准）** ——
> Windows 侧与 Mock A35 全部实现并通过测试（135/135）；A35 实机实现须以本文档
> 为准进行对接。**证据等级**：实机对接完成前，验证结论仅代表"Windows 侧与
> Mock A35 通过"，不得宣称"实机通过"。
> 参考语义：`ROV_A35_M33_Control_Protocol_v1.0.md`（仅业务语义，非 wire 格式）。

## 0. 架构原则（红线）

1. **Windows 与 M33 双盲**：Windows 只理解 Windows↔A35 业务协议，不生成 M33
   ASCII 命令、不感知 M33 四位 SEQ、不访问 `/dev/ttyRPMSG0`、不依赖 M33 源码。
2. **A35 是唯一协议转换与状态权威**：所有 Windows 业务请求由 A35 转换为
   RovControl/M33 指令、完成通道映射与安全裁决，并返回权威状态；Windows
   本地点击只产生目标状态与 Pending，不当作成功。
3. 所有 funcId / 通道号 / 协议字节集中定义在 `WireConstants` 与
   `FunctionRegistry`，禁止散落在 UI 代码。
4. 本终端的 CH10–CH15 等通道号仅为 Windows↔A35 协议规范 ID，
   与 A35→M33 的实际映射由 A35 决定，Windows 不做任何假设。
5. **版本与演进规则**：本文档为最终版，既有 funcId 数值、载荷与位义不再变更；
   后续新增函数使用未占用 ID 区段；权威状态扩展时递增 StateEventV2 的
   version 字段（未知版本整帧拒绝，保证前向兼容）；任何变更须双方确认并
   同步更新本文档与 Windows 侧 `WireConstants`。

## 1. 帧格式（小端、逐字段序列化，与 v1 相同）

```
偏移  字段     宽度  说明
0     magic    u32   0x53414C41
4     version  u8    1
5     funcId   u16   函数 ID（见 §2）
7     seq      u16   Windows 侧生成递增（0-65535 回绕）；ACK/数据响应原样回带
9     flags    u8    bit0=需ACK bit1=事件帧(板端主动) bit2=错误响应
10    len      u16   payload 字节数（≤ max_payload）
12    payload  len   逐字段小端序列化
12+len crc16    u16   CRC16-CCITT-FALSE（初值 0xFFFF，多项式 0x1021），覆盖偏移 0..11+len
```

接收侧处置：半包/粘包自动分帧；坏 magic/版本/CRC 重同步；超长帧或缓存溢出
（`[tcp] max_payload / recv_buffer_limit`）判定失步断线重连；未知 funcId
计数告警不断线。

## 2. 执行器 ID 规范（v2 新增，UI 编号/wireId 分离）

| 类别 | wireId | UI 编号 | 推荐标签 |
|------|--------|---------|----------|
| 舵机×10 | 0–9 | 1–10 | 舵机1（CH0）… 舵机10（CH9） |
| 垂直推进器×4 | 10–13 | 1–4 | 垂直1（CH10）… 垂直4（CH13） |
| 水平推进器×2 | 14–15 | 1–2 | 水平1（CH14）、水平2（CH15） |

- `set servo`/`get servo`/`set servo mid`：id ∈ [0,9]；"全部"选择子为
  `0xFF`（`set servo all` 仍为独立 funcId）。
- `set propeller`：id ∈ [10,15]，无广播语义；`set propeller stop`：
  id ∈ [10,15] 或 `0xFF`=全部。
- 非法 ID 一律在 Windows 编码器拒绝（返回空载荷不发送）。
- 旧 ini 键 `[control] servo_count/thruster_count/id_base` 已弃用
  （存在即告警），拓扑以 `WireConstants` 常量为唯一权威。

## 3. 函数注册表（42 条）

| funcId | 名称 | 方向 | 载荷 | ACK | 优先级 | 说明 |
|--------|------|------|------|-----|--------|------|
| 0x0001 | ask | Win→A35 | 空 | 是 | 5 | 在线探测 |
| 0x0002 | ver | Win→A35 | 空 | 是 | 5 | 版本查询 |
| 0x0003 | status | Win→A35 | 空 | 是 | 5 | 系统状态 |
| 0x0004 | help | Win→A35 | 空 | 是 | 5 | 支持命令查询 |
| 0x0010 | stop all | Win→A35 | 空 | 是 | 2 | 六路推进器置零并进入停止状态（推进器总使能 OFF 锁存；复用原 stop funcId） |
| 0x0011 | emergency | Win→A35 | 空 | 是 | 1 | 紧急停机：**仅六路推进器置零，不上浮、不操作舵机** |
| 0x0012 | estop | Win→A35 | 空 | 是 | 0 | 一级紧急停机：执行结果与 stop all 完全相同，仅优先级/GUI 告警等级/日志事件类型不同；**不携带任何执行器载荷** |
| 0x0013 | move all | Win→A35 | 空 | 是 | 2 | 解除全局停止锁存（推进器总使能 ON） |
| 0x0014 | stop vertical | Win→A35 | 空 | 是 | 2 | 垂直组停止锁存（垂直推进使能 OFF） |
| 0x0015 | move vertical | Win→A35 | 空 | 是 | 2 | 解除垂直组停止锁存（垂直推进使能 ON） |
| 0x0016 | stop horizontal | Win→A35 | 空 | 是 | 2 | 水平组停止锁存（水平推进使能 OFF） |
| 0x0017 | move horizontal | Win→A35 | 空 | 是 | 2 | 解除水平组停止锁存（水平推进使能 ON） |
| 0x0020/21 | safe on/off | Win→A35 | 空 | 是 | 5 | 安全保护模式开关（见 §6 联动） |
| 0x0022/23 | horizontal on/off | Win→A35 | 空 | 是 | 5 | 姿态稳定（Horizontal，Roll/Pitch 自动调平）开关；**不代表 CH14–15 水平推进器** |
| 0x0024/25 | vertical synchronization on/off | Win→A35 | 空 | 是 | 5 | 垂直组同步开关 |
| 0x0026/27 | horizontal synchronization on/off | Win→A35 | 空 | 是 | 5 | 水平组同步开关 |
| 0x0030 | set servo | Win→A35 | u8 id(0–9) + u16 angle(0-180) | 是 | 5 | 单舵机 |
| 0x0031 | set servo all | Win→A35 | u16 angle | 是 | 5 | 全部舵机同角 |
| 0x0032 | set servo mid | Win→A35 | u8 id(0–9 或 0xFF=all) | 是 | 5 | 归中 90° |
| 0x0033/34 | get servo (id/all) | Win→A35 | u8 id / 空(all) | 是 | 5 | 响应 u16[] 角度 |
| 0x0040 | set propeller | Win→A35 | u8 id(10–15) + i16 pct(-100..100) | 是 | 5 | 单推进器基准 |
| 0x0041 | set propeller all | Win→A35 | i16 pct | 是 | 5 | 全部同值 |
| 0x0042 | set propeller stop | Win→A35 | u8 id(10–15 或 0xFF=all) | 是 | 5 | 基准归零 |
| 0x0043/44 | get propeller base/real | Win→A35 | u8 id(10–15) | 是 | 5 | 响应 i16[] |
| 0x0050 | base value | Win→A35 | — | 是 | 5 | **[弃用]** 旧 6×i16 同值基准，勿再使用 |
| 0x0051 | base value vh | Win→A35 | 2×i16：垂直基准、水平基准 | 是 | 5 | 姿态稳定统一基准单帧（-100..100） |
| 0x0060/61/62 | sensor mpu/dyp/all | Win→A35 | 空 | 是 | 5 | 传感器查询 |
| 0x00F0 | heartbeat | Win→A35 | u32 客户端毫秒时间戳 | 否 | 5 | 应用层保活心跳：无需 ACK，A35 可回显或静默丢弃（链路活性由 Windows 发送侧超时判定；周期 `[tcp] heartbeat_interval_ms`） |
| 0x0100 | sensor summary | A35→Win | 45B（见 §4） | — | — | 100Hz 传感器汇总 |
| 0x0101 | ack | A35→Win | u16 errCode | — | — | ACK（回带 seq，见 §7） |
| 0x0102 | state event | A35→Win | u8 掩码 | — | — | **[legacy]** 4 位掩码，位义冻结，仅兼容回退（见 §5） |
| 0x0103 | alarm event | A35→Win | u8 level + u16 code + u32 boardTimeMs + UTF-8 text | — | — | 告警事件 |
| 0x0104 | state event v2 | A35→Win | u8 version=2 + u16 mask | — | — | 权威状态事件（主链路，见 §5） |

发送优先级（小者优先）：`Estop(0) > Emergency(1) > Stop/Move(2) > 普通(5)`。
紧急队列（priority<5）按优先级稳定排序插入且永不因溢出丢弃。

## 4. 传感器汇总帧（0x0100，45 字节，与 v1 相同）

```
偏移  字段            类型
0     tempC           f32
4     humidPct        f32
8     accelMps2[3]    3×f32
20    gyroRadS[3]     3×f32
32    voltage         f32
36    distMm          f32（DYP-RD；无效哨兵 -1.0）
40    validMask       u8（bit0 温湿 bit1 MPU bit2 电压 bit3 DYP）
41    boardTimeMs     u32（低 32 位）
```

NaN/Inf 拒绝整帧；有效位未置的字段 UI 显示"无效"（不用 0 冒充）。

## 5. 权威状态事件

### 5.1 StateEventV2（0x0104，主链路）

```
u8  version = 2
u16 mask（小端）：
  bit0 safe                      bit1 attitudeStabilization（姿态稳定）
  bit2 globalStopped             bit3 verticalStopped
  bit4 horizontalStopped         bit5 verticalSynchronization
  bit6 horizontalSynchronization bit7 estop
  bit8 emergency
```

- `stopped` 位=1 表示该组处于停止锁存；终端"使能"开关显示取反值
  （如 globalStopped=0 → 推进器总使能显示 ON）。
- 未知位（bit9–15 置位）或版本不符整帧拒绝（禁止静默按旧位义解读）。
- 版本字段用于前向兼容：A35 后续扩展状态时递增版本并协商。

### 5.2 legacy StateEvent（0x0102，兼容回退）

u8 掩码：bit0=safe bit1=horizontal(姿态稳定) bit2=estop bit3=emergency
（位=1 激活）。位义冻结不改；仅携带 4 位，使能/同步开关保持原状态
（无事件依据不臆造，终端保守禁用推进器直至获得 V2 事件）。

## 6. 业务语义（协议最终版，A35 实现须遵守）

### 6.1 Safe 与姿态稳定单向联动

- Safe 为 A35 侧保护策略：不是 Stop、不是 Emergency、不控制舵机、
  不自动把推进器置零；在姿态稳定基础上执行边界检查/限幅/拒绝，
  最终裁决由 A35 负责（Windows 不做安全权威）。
- 联动规则：`Safe ON => 姿态稳定必须 ON`；SafeOff 不自动关闭姿态稳定；
  Safe ON 期间终端禁止关闭姿态稳定（UI 锁定+说明）。
- SafeOn 仅发一条业务级 0x0020（Windows 不拼接两条命令）；A35 应原子地
  保证姿态稳定开启后再确认 Safe 成功。非法权威组合
  `Safe=ON 且 姿态稳定=OFF` 到达时：终端锁定推进器并产生高等级告警，
  不在 Windows 端悄悄修正。

### 6.2 Stop/Move 三级使能（锁存语义）

- 总使能（move all/stop all）、垂直使能、水平使能：ON=允许运动，
  OFF=停止锁存；总使能 OFF 期间终端禁止发起分组 Move。
- 推进器实际可操作条件：TCP 已连接 + 权威状态已知 + 总使能 ON +
  对应分组使能 ON + 相关节点非 Pending。
- 重新使能只恢复未被分组 Stop 锁定的组，**不自动恢复或重发旧推进器目标**。
- Stop/Move 不改变 Safe、姿态稳定、Synchronization、舵机状态。

### 6.3 Stop / Estop / Emergency

- 三者执行结果完全相同：六路推进器置零并进入停止状态；**均不得向舵机
  发送任何指令，载荷为空，绝不携带舵机角度**。
- 区别仅允许体现在：调度优先级（§3）、GUI 告警等级、日志事件类型、
  Estop 权威状态展示。

### 6.4 Synchronization（垂直/水平两组独立）

- 同步开关只影响终端布局与提交方式（同步=单条滑条控制全组同值），
  对 A35 仍是逐通道 `set propeller`（wire 10–15）；主页与指令页绑定
  同一状态模型。

### 6.5 舵机权限

- 舵机可操作条件仅为：TCP 连接存在、命令可发送、通道/角度参数合法、
  当前通道无冲突 Pending。Safe/姿态稳定/同步/推进器停止锁存/Estop/
  Emergency 均不置灰舵机、不改变舵机目标。

## 7. 请求生命周期与 ACK

- 全部 Request 帧带 NeedAck；ACK（0x0101）回带 seq 与 u16 errCode。
- 开关类请求（safe/姿态稳定/使能/同步）：成功 ACK 提交目标状态；
  失败 ACK/超时/断线回退发起前权威状态，并产生窗口级提示与告警记录。
- StateEventV2 先于 ACK 到达：以事件为权威并清除对应 Pending；
  成功 ACK 后事件给出相反状态：事件覆盖并产生协议不一致高等级告警。
- 超时（`request_timeout_ms`）显示"状态未知"，不自动重发；
  迟到响应计数丢弃；断开清空队列与挂起（不重放危险指令），
  首连后掉线期间拒绝入队；重连成功自动 ask/status/sensor all 只读查询。

## 8. 错误码（最终版）

| errCode | 含义 |
|---------|------|
| 0 | ok |
| 1 | bad_cmd |
| 2 | bad_arg |
| 3 | busy |
| 4 | not_ready |
| 5 | timeout |
| 6 | unsupported |
| 7 | safety（安全状态不允许） |

## 9. 行为契约（Windows 侧已实现）

- 发送：双优先级队列（紧急通道按优先级稳定排序、永不丢弃）；
  `slider_rate_limit_ms` 限频合并 + 松手立即冲刷；分组停止锁存期间
  丢弃该组待发、重新使能不重放；
- 重连：指数退避（base×2^n 封顶，幂次钳位防溢出）；
- 心跳：可配开关与周期；
- 视频：单一 GStreamer 管线/单 UDP 端口，显示帧经 VideoFrameHub
  最新帧发布层供主页与指令页共享（本条为终端内部行为，与 A35 无关）。

## 10. A35 实机实现要求（协议已定稿，按本文档对接）

协议字段至此全部定稿（**不再有"待确认"项**）；以下为 A35 侧实机实现的
强制要求与对接检查单（Windows 侧已全部就绪，Mock A35 已按本档验证）：

1. 帧格式按 §1 逐字段实现（magic/version/flags/CRC16-CCITT-FALSE），
   半包/粘包/坏帧处置见 §1；
2. 函数表按 §3 全 42 条实现，funcId 数值即最终分配（含本轮新增
   0x0013–0x0017、0x0024–0x0027、0x0051、0x0104）；全部 Request 须按
   needsAck 回 0x0101（回带 seq + errCode）；
3. StateEventV2（0x0104）：**连接建立后须主动推送一次全量状态**，此后任一
   状态位变化须再次推送（推荐 1Hz 周期兜底）；掩码位义按 §5.1，
   stopped 位与 Stop/Move 锁存一一对应；
4. SafeOn 须原子执行：**先保证姿态稳定开启，再确认 Safe 成功**；板端不得
   产生/维持 Safe=ON 且姿态稳定=OFF 的状态（Windows 将其判为非法权威组合
   并锁定推进器 + 高等级告警）；
5. Stop/Estop/Emergency 均为六路推进器置零：不向舵机发送任何指令、不携带
   执行器载荷、执行结果完全相同（仅调度优先级不同）；
6. 传感器汇总（0x0100）按 §4 45 字节定长，20–100Hz；DYP-RD 单位为 mm、
   无效哨兵 -1.0；NaN/Inf 由 Windows 拒帧，A35 侧不应产生；
7. 心跳（0x00F0）：无需 ACK，A35 可回显或静默丢弃；
8. 服务端口 7000（Windows 侧 `[tcp] port` 可配）；错误码按 §8；
9. 值域：舵机 0–180°、推进器 ±100%（越界 A35 应回 errCode=2 bad_arg）。

**对接遗留（非协议项）**：电池化学体系与 SOC 分段曲线参数由用户提供后填入
`[battery] soc_curve`（与协议无关）；实机联调通过后更新本文档证据等级表述。
