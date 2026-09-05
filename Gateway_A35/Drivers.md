在 STM32MP257DAK3/正点原子 ATK-DLMP257B 的 A35 OpenSTLinux 端，实现 DHT11 温湿度与 INA226 电压、电流、功率采集功能，并接入现有 A35 网关程序。

## 一、项目边界

1. 不修改 M33 固件。
2. DHT11、INA226 由 A35 本地采集，不通过 RPMsg 请求 M33。
3. Windows 与 M33 保持双盲：
   - Windows 只连接 A35。
   - A35 负责本地传感器采集和 Windows 协议响应。
   - 只有推进器、舵机等实时控制命令才按现有协议转发给 M33。
4. 不重新定义 A35/M33 控制协议。
5. 若 Windows-A35 协议尚未定义 DHT11/INA226 字段，本阶段先实现采集服务和内部数据接口，不擅自新增线上命令。
6. 不下载或替换内核源码，不修改设备树和系统启动配置；如果 INA226 缺少设备树节点，只输出所需修改建议。

## 二、参考源码

ST 官方 Linux 仓库：

https://github.com/STMicroelectronics/linux/tree/v6.6-stm32mp

ST/Linux 标准 DHT11 IIO 驱动：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/drivers/iio/humidity/dht11.c

DHT11 Kconfig：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/drivers/iio/humidity/Kconfig

DHT11 设备树绑定：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/Documentation/devicetree/bindings/iio/humidity/dht11.yaml

ST/Linux INA2xx HWMON 驱动：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/drivers/hwmon/ina2xx.c

INA2xx Kconfig：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/drivers/hwmon/Kconfig

INA2xx 设备树绑定：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/Documentation/devicetree/bindings/hwmon/ti%2Cina2xx.yaml

INA2xx 用户空间接口说明：

https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/Documentation/hwmon/ina2xx.rst

注意：当前正点原子出厂固件使用自定义 `drivers/char/dht11.c`，其接口是 `/sys/class/misc/dht11/value`，不是 ST/Linux 标准 IIO DHT11 接口。当前程序必须优先兼容出厂固件。

## 三、开发前检查

先在目标板执行只读检查：

```sh
uname -a
modinfo dht11
modinfo ina2xx

ls -l /sys/class/misc/dht11/value
grep -H . /sys/class/hwmon/hwmon*/name 2>/dev/null
dmesg | grep -Ei 'dht11|ds18b20|ina2|i2c'
```

记录实际接口、模块版本、INA226 的 I²C 总线和地址。不得臆测总线号、I²C 地址及采样电阻。

## 四、DHT11 调用方式

开发板手册给出的驱动准备命令为：

```sh
modprobe -r ds18b20
modprobe -r dht11
modprobe dht11
cat /sys/class/misc/dht11/value
```

模块切换属于部署或人工准备步骤。正式 A35 程序不得在每次采样时调用 `modprobe`、`rmmod` 或 `cat`，而应直接打开并读取：

```text
/sys/class/misc/dht11/value
```

实现 `Dht11Reader`：

- 默认采样周期为2秒，可配置但不得短于1秒。
- 每次采样重新打开、读取并关闭 sysfs 文件。
- 去除首尾空白，同时保存原始字符串用于诊断。
- 正常四位数据按“前两位湿度、后两位温度”解析，例如 `6929` 表示 `69%RH、29°C`。
- 三位或两位数据可能缺少前导零，存在歧义：
  - 默认不得静默猜测。
  - 三位数据可提供可配置的手册兼容策略：前两位作为湿度，剩余一位作为温度；使用该策略时必须设置 `inferred=true` 并记录警告。
  - 两位或无法唯一判断的数据返回 `AMBIGUOUS_FORMAT`，保留原始值。
- 校验温度和湿度范围；非法数据不得进入正常遥测。
- 文件不存在时返回 `NOT_FOUND`，并提示检查 DS18B20 是否占用了共用 GPIO。
- 读取失败、驱动超时、校验失败时不得使整个 A35 网关退出。

同时预留标准 IIO 后端，但不要改变当前默认选择。标准后端应扫描：

```text
/sys/bus/iio/devices/iio:device*/name
```

找到 DHT11 后读取：

```text
in_temp_input
in_humidityrelative_input
```

标准驱动数值通常以千分单位表示，转换前必须根据实际 ABI 和驱动源码确认。不要写死 `iio:deviceX` 编号。

## 五、INA226 调用方式

INA226 使用 `ina2xx` HWMON 驱动。首先加载并检查：

```sh
modprobe ina2xx
grep -H . /sys/class/hwmon/hwmon*/name 2>/dev/null
```

正式程序扫描 `/sys/class/hwmon/hwmon*`，读取各目录的 `name`，匹配 `ina226` 或经实际验证的 `ina2xx` 名称，不得写死 `hwmon0` 等编号。

实现 `Ina226Reader`，读取：

```text
in1_input
in0_input
curr1_input
power1_input
shunt_resistor
update_interval
```

换算规则：

- `in1_input`：总线电压，mV；除以1000得到V。
- `in0_input`：采样电阻压降，mV；允许负值。
- `curr1_input`：电流，mA；除以1000得到A，允许负值。
- `power1_input`：功率，µW；除以1000000得到W。
- `shunt_resistor`：采样电阻，µΩ。
- `update_interval`：驱动转换更新周期，ms。

所有数值使用有符号64位整数读取，再转换为浮点工程单位，防止溢出和负电流解析错误。

如果找不到 INA226：

1. 检查 `ina2xx` 模块是否加载。
2. 检查 I²C 设备是否被探测。
3. 检查设备树是否存在 `compatible = "ti,ina226"` 节点。
4. 检查实际 I²C 地址和采样电阻。
5. 输出明确诊断，不使用运行时 `new_device` 代替正式设备树配置。

仅插入 INA226 模块但没有正确设备树/I²C 绑定时，不会自动出现 `hwmon` 接口。

## 六、采集服务设计

建立独立 `SensorService`：

- DHT11 默认每2秒采集一次。
- INA226 默认每500毫秒采集一次，周期可配置。
- 采集线程不得阻塞 TCP/UDP、RPMsg或控制命令处理线程。
- 为每种传感器保存最后一次成功样本、采样时间、错误状态和连续失败次数。
- 数据超过三个采样周期未更新时标记为 `stale`。
- 传感器拔出、驱动重载或 sysfs 编号变化后，自动重新扫描接口。
- 错误日志限频，避免传感器缺失时持续刷屏。

建议数据结构至少包含：

```text
Dht11Sample:
  temperature_c
  humidity_percent
  raw_value
  timestamp
  status
  inferred
  error_message

Ina226Sample:
  bus_voltage_v
  shunt_voltage_mv
  current_a
  power_w
  shunt_resistor_uohm
  timestamp
  status
  error_message
```

A35 响应 Windows 的传感器查询时，应直接返回缓存样本，不在网络处理线程中现场执行慢速 DHT11 采样。

## 七、工程实现

- 优先遵循现有 A35 项目的语言、目录和构建系统。
- 如果 A35 项目尚未建立，使用 C++17、CMake 和标准 Linux 文件接口。
- 不通过 `system()`、`popen()` 或启动 shell 的方式读取传感器。
- sysfs 根目录应可注入，例如测试时设置为临时目录。
- 配置项至少包括：
  - DHT11 接口模式：`alientek_misc`、`standard_iio`、`auto`
  - DHT11 采样周期
  - INA226 采样周期
  - 三位 DHT11 数据的兼容解析策略
- `auto` 模式优先使用正点原子 Misc 接口，其次尝试标准 IIO 接口。

## 八、测试要求

在没有真实传感器时，使用虚拟 sysfs 目录完成单元测试：

- DHT11 正常四位数据。
- DHT11 三位推断数据。
- DHT11 两位歧义数据。
- 空文件、非数字、越界数据、文件不存在。
- INA226 正常值、负电流、缺少部分属性。
- `hwmonX` 编号变化。
- 传感器运行中消失及重新出现。
- 连续失败、恢复、数据过期状态。
- 保证传感器故障不会影响 Windows-A35 网络连接和 A35-M33 控制链路。

## 九、完成报告

完成后报告：

1. 修改和新增的文件。
2. DHT11、INA226 实际使用的内核接口。
3. 构建和测试结果。
4. 无硬件测试覆盖情况。
5. 仍需实物确认的 I²C 总线、地址、采样电阻和数据单位。
6. 是否需要设备树变更。
7. 明确确认未修改 M33 固件及 A35/M33 正式控制协议。