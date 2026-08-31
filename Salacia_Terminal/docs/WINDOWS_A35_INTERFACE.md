# Windows↔A35 TCP 接口说明（草案 v1，待 A35 确认）

> 本文档描述 Salacia_Terminal（Windows 侧）实现的 TCP 控制通道。
> **状态：草案** —— 实机对接前所有字段需 A35 团队逐项确认（清单见文末）。
> 参考语义：`ROV_A35_M33_Control_Protocol_v1.0.md`（仅业务语义，非 wire 格式）。

## 1. 帧格式（小端、逐字段序列化）

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
（`[tcp] max_payload / recv_buffer_limit`）判定失步断线重连；未知 funcId 计数告警不断线。

## 2. 函数注册表（31 条）

| funcId | 名称 | 方向 | 载荷 | ACK | 说明 |
|--------|------|------|------|-----|------|
| 0x0001 | ask | Win→A35 | 空 | 是 | 在线探测 |
| 0x0002 | ver | Win→A35 | 空 | 是 | 版本查询 |
| 0x0003 | status | Win→A35 | 空 | 是 | 系统状态 |
| 0x0004 | help | Win→A35 | 空 | 是 | 支持命令 |
| 0x0010 | stop | Win→A35 | 空 | 是 | 正常停止（水平归零，垂直维持） |
| 0x0011 | emergency | Win→A35 | 空 | 是 | 受控紧急上浮 |
| 0x0012 | estop | Win→A35 | 32B：10×u16 舵机零值+6×i16 推进器零值 | 是 | 一级紧急停机（单帧） |
| 0x0020/21 | safe on/off | Win→A35 | 空 | 是 | 安全模式开关 |
| 0x0022/23 | horizontal on/off | Win→A35 | 空 | 是 | 水平/姿态补偿开关 |
| 0x0030 | set servo | Win→A35 | u8 id + u16 angle(0-180) | 是 | 单舵机（id 0=all 不可用于此） |
| 0x0031 | set servo all | Win→A35 | u16 angle | 是 | 全部舵机同角 |
| 0x0032 | set servo mid | Win→A35 | u8 id(0=all) | 是 | 归中 90° |
| 0x0033/34 | get servo (id/all) | Win→A35 | u8 id | 是 | 响应 u16[] 角度 |
| 0x0040 | set propeller | Win→A35 | u8 id + i16 pct(-100..100) | 是 | 单推进器基准 |
| 0x0041 | set propeller all | Win→A35 | i16 pct | 是 | 全部同值 |
| 0x0042 | set propeller stop | Win→A35 | u8 id(0=all) | 是 | 基准归零 |
| 0x0043/44 | get propeller base/real | Win→A35 | u8 id | 是 | 响应 i16[] |
| 0x0050 | base value | Win→A35 | 6×i16 同值 | 是 | horizontal on 统一基准单帧 |
| 0x0060/61/62 | sensor mpu/dyp/all | Win→A35 | 空 | 是 | 传感器查询 |
| 0x00F0 | heartbeat | Win→A35 | u32 时间戳 | 否 | 心跳（机制待确认） |
| 0x0100 | sensor summary | A35→Win | 45B（见 §3） | — | 100Hz 传感器汇总 |
| 0x0101 | ack | A35→Win | u16 errCode | — | ACK（回带 seq） |
| 0x0102 | state event | A35→Win | u8 掩码（见 §4） | — | 模式权威状态 |
| 0x0103 | alarm event | A35→Win | 待定义 | — | 告警事件 |

## 3. 传感器汇总帧（0x0100，45 字节）

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

## 4. 状态事件（0x0102）

u8 掩码：bit0=safe bit1=horizontal bit2=estop bit3=emergency（位=1 激活）。
未知位整帧拒绝。终端权限矩阵以此为准。

## 5. 错误码（草案，对齐协议文档返回码）

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

## 6. 行为契约（Windows 侧已实现）

- 发送：双优先级队列（estop/emergency 插队且永不因溢出丢弃）；`slider_rate_limit_ms`
  限频合并 + 松手立即冲刷；断开即清队，首连后掉线期间拒绝入队（不重放）；
- 重连：指数退避（base×2^n 封顶 max，幂次钳位防溢出），成功后自动 ask/status/sensor all 只读权威查询；
- ACK：seq 匹配，超时（`request_timeout_ms`）显示"状态未知"不自动重发；迟到响应计数丢弃；
- 心跳：可配开关与周期。

## 7. 待 A35 确认项

1. 帧格式全字段（magic/version/flags/CRC 算法）
2. estop 专用 funcId 与 ACK 行为
3. 100Hz 帧字段/有效位/哨兵值
4. DYP-RD wire 单位（暂定 mm）/量程/无效值
5. 电池化学体系与 SOC 曲线参数
6. 心跳机制（应用层 or TCP keepalive）
7. 错误码表（§5 为草案）
8. A35 服务端口（终端默认 7000，`[tcp] port`）
9. ACK/迟到响应/超时语义
10. servo/propeller 值域确认（0-180° / ±100%）
