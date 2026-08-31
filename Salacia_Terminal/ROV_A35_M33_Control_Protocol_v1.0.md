> 这是 **ROV A35/M33 Control Protocol v1.0** 当前正式协议基准版。

# ROV A35/M33 控制指令集规范 v1.0

基于已验证 OpenAMP/RPMsg 链路的 ROV 应用层控制协议

版本状态：v1.0 / 协议基准版

目的：在不改变既有 OpenAMP/RPMsg 基础设施的前提下，建立统一的 A35/Linux ↔ M33/FreeRTOS 应用层控制、状态回传与安全控制接口。

本版依据前两版草案完成收敛，用户命令统一小写、SEQ 由 A35/Linux 侧自动生成，并正式确定 stop / safe / emergency 三者语义。

## 1. 设计原则

- 复用已经验证的 /dev/ttyRPMSG0 通信链路，不重新设计异核通信底层。
- Linux/A35 发送业务意图，M33 负责具体硬件动作；Linux 不直接操作 M33 HAL。
- 命令协议采用可读 ASCII 文本，便于 SSH/Linux 命令行直接测试；后续如有性能需求可演进为二进制协议。
- 协议关键字、子命令、模式名和正式工具输出统一使用小写；M33 接收端允许大小写不敏感解析。
- SEQ 由 A35/Linux 侧的 ctl 自动递增，用户不手工填写；M33 对 ACK、错误和数据响应原样返回 SEQ。
- 运动控制必须具备明确的正常停止、保护模式和紧急脱险机制。
- 通信超时、看门狗、关键传感器异常或系统故障时，不得无限期保持最后一次危险推进器命令。
- 传感器和执行器通过服务层暴露能力，底层 GPIO/I2C/UART/PWM 实现对上层透明。

## 2. 系统结构

```text
A35/Linux shell / ctl
        ↓ /dev/ttyRPMSG0
M33 RPMsg 接收
        ↓
Command Parser → Command Dispatcher
        ├── ControlTask
        ├── SensorTask
        ├── ActuatorTask
        └── Status/Event
        ↓
A35/Linux
```

## 3. 协议格式

### 3.1 用户命令

```text
<command> [args...]
```

示例：

```text
ctl ask
ctl status
ctl set servo 2 90
ctl sensor mpu
ctl stop
ctl emergency
```

### 3.2 Wire format

| 层级 | 格式 | 示例 | 说明 |
|---|---|---|---|
| 用户输入 | `<command> [args...]` | `set servo 2 90` | 用户无需 SEQ |
| ctl → M33 | `<seq> <command> [args...]` | `0002 set servo 2 90` | SEQ 自动生成 |
| M33 → A35 | `<seq> <result> [data...]` | `0002 ok` | ACK/错误/数据必须回带相同 SEQ |
| 主动事件 | `event <event> [args...]` | `event ready` | M33 主动产生，不要求 SEQ |

字段约定：关键字、子命令和模式名使用小写；数值与设备 id 使用十进制；字段之间允许一个或多个空白字符。

为便于底层手工调试，M33 应兼容无 SEQ 的直接输入；正式应用始终建议通过 ctl 发送。

## 4. 系统命令

| 命令 | 用途 | 用户示例 | 典型返回 |
|---|---|---|---|
| `ask` | 测试 M33 是否在线 | `ctl ask` | `0001 online` |
| `ver` | 查询 M33 固件版本 | `ctl ver` | `0002 version 1.0` |
| `status` | 查询系统/任务/设备状态 | `ctl status` | `0003 ok ...` |
| `help` | 查询支持命令 | `ctl help` | `0004 ok ...` |

## 5. 安全与模式命令

### 5.1 stop — 正常停止当前运动

定义：让 ROV 尽可能保持“原地不动”。

- 水平推进器：输出归零。
- 垂直推进器：进入中性浮力/原地维持所需的控制状态，不简单归零。
- 不得因为 stop 直接主动上浮；是否保持深度由垂直控制策略决定。
- 舵机保持当前位置，除非其他安全策略另有规定。

用户命令：

```text
ctl stop
```

建议返回：

```text
0005 ok stopped
```

### 5.2 emergency — 紧急上浮

定义：放弃当前正常任务，立即进入紧急脱险状态。

- 水平推进器：输出归零。
- 垂直推进器：进入受限的紧急上浮推力。
- 紧急上浮推力为“最大允许安全上浮推力”，不等同于电机/电调的物理最大值。
- 在上浮过程中继续保留 IMU 姿态修正，以抑制 Roll/Pitch；姿态修正不得使总体上浮能力失去。
- 进入 emergency 后，普通运动控制命令默认拒绝，直至系统按规定退出该状态。

用户命令：

```text
ctl emergency
```

建议返回：

```text
0006 ok emergency
```

### 5.3 safe — 安全保护模式

定义：safe 是系统保护状态，而不是单一运动动作。其目标是限制危险控制，而不是自动决定 ROV 上浮或下沉。

- 开启水平限制（horizontal control）。
- 检查每个推进器的“基准转速”是否处于安全范围；超出范围时，将基准值强制限制到对应安全阈值。
- 安全范围检查以推进器基准转速为准，而不是检查经过水平 PID 等实时微调后的最终真实转速。
- 因此，如果基准值为 1900 us，而因水平 PID 微调导致真实输出暂时达到 2000 us，不应因为该 2000 us 直接判定为越界并强行改变 PID 输出。
- safe 不自动等同于 stop，也不自动等同于 emergency；进入 safe 后具体推进器输出由当前控制策略继续管理，但危险命令应受到限制。
- 通信超时、看门狗、关键传感器异常或系统故障时，可自动进入 safe。

用户命令：

```text
ctl safe on
ctl safe off
```

建议返回：

```text
0007 ok safe_on
0008 ok safe_off
```

## 6. 水平控制命令

| 命令 | 功能 | 用户示例 | 典型返回 |
|---|---|---|---|
| `horizontal on` | 启用水平/姿态自动补偿；用户只调整所有推进器的基准转速，程序对单个推进器做微调 | `ctl horizontal on` | `ok horizontal_on` |
| `horizontal off` | 关闭水平自动补偿，允许手动单独调整各推进器 | `ctl horizontal off` | `ok horizontal_off` |

水平控制开启时，推进器命令中的 value 表示基准转速；实际 PWM 由基准值与控制算法的微调量共同决定。

## 7. 舵机命令

| 命令 | 功能 | 说明 |
|---|---|---|
| `set servo <id> <angle>` | 控制单个舵机 | angle 为 0–180° |
| `set servo all <angle>` | 控制所有舵机 | angle 为 0–180° |
| `get servo <id>` | 查询单个舵机角度 | |
| `get servo all` | 查询所有舵机角度 | |
| `set servo <id> mid` | 单个舵机归中 | 等价于 `set servo <id> 90` |
| `set servo all mid` | 所有舵机归中 | |

LU9685 以及后续 PCA9685 均封装于 ActuatorService；协议层不直接暴露 I2C/PWM 寄存器。

## 8. 推进器命令

| 命令 | 功能 | 说明 |
|---|---|---|
| `set propeller <id> <value>` | 设置单个推进器基准转速 | value：-100～+100 |
| `set propeller all <value>` | 设置所有推进器基准转速 | value：-100～+100 |
| `get propeller <id> base` | 查询单个推进器基准转速 | |
| `get propeller <id> real` | 查询单个推进器实时转速 | |
| `get propeller all base` | 查询所有推进器基准转速 | |
| `get propeller all real` | 查询所有推进器实时转速 | |
| `set propeller <id> stop` | 停止单个推进器 | 等价于设置该推进器基准值为 0 |
| `set propeller all stop` | 停止所有推进器 | 设置所有推进器基准值为 0 |

方向、死区、限幅、安全边界以及最终 PWM 映射由 M33 控制层处理。safe 模式对基准转速实施安全限幅。

## 9. 传感器查询

| 命令 | 作用 | 典型数据 |
|---|---|---|
| `sensor mpu` | 读取 MPU6500 当前数据 | ax/ay/az/gx/gy/gz |
| `sensor dyp` | 读取 DYP 测距结果 | distance_mm |
| `sensor all` | 查询 M33 侧传感器状态 | ready/error/timeout |

## 10. A35/Linux 侧设备

- INA226：由 Linux I2C3 + ina2xx + hwmon 管理；已实际验证 VBUS 约 4.955 V。
- DHT11：优先使用板卡已有 Linux dht11 驱动，待新硬件到货完成最终实机验证。

因此第一版 M33 协议不强制加入 power/temp 等 M33 硬件查询命令；A35/Linux 可以本地采集并由上层控制程序统一展示。

## 11. M33 → A35 主动事件

| 事件 | 用途 |
|---|---|
| `event ready` | M33 启动完成 |
| `event sensor_ready <name>` | 传感器初始化完成 |
| `event sensor_timeout <name>` | 传感器读取超时 |
| `event servo_error <id>` | 执行器异常 |
| `event safe_stop <reason>` | 系统进入安全保护状态 |

## 12. 返回码

| 返回码 | 含义 |
|---|---|
| `ok` | 命令已接受/执行 |
| `err bad_cmd` | 命令不存在 |
| `err bad_arg` | 参数错误 |
| `err busy` | 设备/任务忙 |
| `err not_ready` | 设备尚未就绪 |
| `err timeout` | 操作超时 |
| `err unsupported` | 暂未实现 |
| `err safety` | 当前安全状态不允许执行 |

## 13. FreeRTOS Task 对应关系

| Task | 职责 | 建议优先级 |
|---|---|---|
| CommTask | 接收 RPMsg、解析命令、发送 ACK/数据 | 高 |
| ControlTask | 姿态/推进器控制、模式状态机 | 最高实时优先级 |
| SensorTask | MPU6500、DYP；DHT11 若最终留在 M33 则加入 | 按设备频率 |
| ActuatorTask | LU9685/PCA9685/舵机输出，统一仲裁执行器资源 | 高 |
| StatusTask | 周期状态与事件上报 | 低于控制任务 |

## 14. 软件分层

- 应用层：ROV 命令、模式、安全状态。
- 通信层：RPMsg/OpenAMP、消息收发。
- 服务层：SensorService / ActuatorService / ControlService。
- 驱动层：MPU6500 / LU9685 / DYP / GPIO / UART / I2C / PWM。

上层命令不直接调用 HAL；硬件替换时只需调整对应服务/驱动层，不改变 A35/Linux 命令接口。

## 15. Linux ctl 工具

正式运行时使用轻量 ctl：负责生成 SEQ、向 /dev/ttyRPMSG0 发送命令、等待匹配响应，并向用户显示友好结果。

| 用户命令 | 内部发送 | 作用 |
|---|---|---|
| `ctl ask` | `0001 ask` | 测试 M33 在线 |
| `ctl status` | `0002 status` | 查询状态 |
| `ctl set servo 2 90` | `0003 set servo 2 90` | 控制舵机 |
| `ctl sensor mpu` | `0004 sensor mpu` | 读取 IMU |
| `ctl stop` | `0005 stop` | 正常停止 |
| `ctl emergency` | `0006 emergency` | 紧急上浮 |

正式 ctl 的命令行可以继续采用自然的用户语法，例如 `ctl servo set 2 90`；只要内部 wire format 与本协议一致即可。

## 16. SEQ 规则

- SEQ 由 A35/Linux 侧生成，固定为 4 位十进制字符串：0000～9999。
- 每次成功发送一个命令后递增；超过 9999 后回绕到 0000。
- M33 对 ACK、错误和数据响应均原样返回相同 SEQ。
- 主动 event 消息不要求 SEQ。
- 直接手工写入 /dev/ttyRPMSG0 时可以省略 SEQ；M33 应兼容此调试模式，但正式程序推荐始终通过 ctl。

## 17. 通信与安全策略

- M33 必须能够检测 A35 控制通道超时。
- 超时不应无限保持上一条推进器命令。
- 进入 safe 时执行基于“推进器基准值”的安全限幅，并保持水平限制开启。
- 进入 emergency 时立即关闭水平推进器输出并执行受限紧急上浮控制。
- safe、stop、emergency 是三个不同语义：safe 是保护状态，stop 是正常停止动作，emergency 是主动脱险动作。
- 最终失联策略、超时时间、紧急上浮推力和安全 PWM 阈值应在实机浮力/推进器测试后固化为工程参数。

## 18. 当前硬件归属

| 执行环境 | 接口 | 设备 | 状态 |
|---|---|---|---|
| A35/Linux | I2C3 | PCF8563 | 板载 RTC，Linux 已管理 |
| A35/Linux | I2C3 | INA226 | ina2xx/hwmon；已读到约 4.955 V |
| A35/Linux | GPIO/driver | DHT11 | 已有 Linux 驱动；待新硬件验证 |
| M33/FreeRTOS | I2C8 | MPU6500 | 已验证 |
| M33/FreeRTOS | I2C4 | LU9685 | 已验证 |
| M33/FreeRTOS | UART | DYP-L08 | 待最终硬件/电平验证 |
| 排除 | I2C7 | STPMIC2/system regulators | Secure CID1，排除出 ROV 应用路径 |

## 19. v1.0 第一阶段实现验收

- [ ] ask / response
- [ ] ver
- [ ] status
- [ ] help
- [ ] stop
- [ ] emergency
- [ ] safe on / safe off
- [ ] horizontal on / horizontal off
- [ ] set/get servo
- [ ] set/get propeller
- [ ] sensor mpu
- [ ] sensor dyp（硬件未到时允许返回 err not_ready）
- [ ] event ready / error
- [ ] 通信超时进入 safe 的机制

## 20. 后续可扩展项

- 将文本协议演进为固定结构二进制协议（仅在性能/带宽需要时）。
- 增加 heartbeat 命令或固定周期 heartbeat 消息。
- 增加舵机速度、限位、校准等参数。
- 增加姿态保持/自动任务相关控制命令。
- 使网络控制接口与 ctl 使用统一业务语义。
- 将 A35/Linux 侧 INA226、DHT11 数据统一接入高层 UI/状态监控。
