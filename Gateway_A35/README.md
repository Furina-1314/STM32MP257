# rov_gateway（Gateway_A35）

运行于 STM32MP257 ATK-DLMP257B Cortex-A35 / OpenSTLinux 的 ROV 网关服务：
Windows Salacia_Terminal ↔ A35 `rov_gateway` ↔ M33（经 RovControl / `/dev/ttyRPMSG0`）。
Windows 与 M33 双盲，A35 是唯一协议转换者、权威状态与传感器汇聚节点。

## 当前状态：阶段1~6全部完成（2026-09-03）

- 阶段0~2：审查/协议层/核心调度（主机14测试套件）
- 阶段3：传感器融合（虚拟sysfs全套）
- 阶段4：实机RPMsg联调（板端ctest 13/13、探针全PASS、Estop 5-7ms）
- 阶段5：OpenSTLinux部署（**开机自启动已验证**、INI配置、systemd、运维手册）
- 阶段6：Windows实机对接（**48项LAN一致性ALL PASS + 真实终端已连接**，
  100Hz汇总流板上抓包确认送达）
- 已知问题：T-01 RPMsg长跑退化（M33侧根因未闭环，轻量轮询缓解，见
  `TROUBLESHOOTING.md`）

设计依据：`PHASE0_REVIEW.md`（只读审查与全量映射）、`DECISIONS.md`（用户批复与设计决策）、
`Salacia_Terminal/docs/WINDOWS_A35_INTERFACE.md`（TCP 协议最终权威）、
`ROV_M33/project-docs/ROV_A35_M33_Control_Protocol_v1.0.md`（A35↔M33 业务语义）。

## 当前状态：阶段5（OpenSTLinux部署）已完成

阶段1~5 完成。主机 14 套件全过；板端 ctest 13/13；**开机自启动全链路上板验证**
（R-06）；服务生命周期（restart/kill-9/stop）验证（R-07）。设计决策与实机证据见
`DECISIONS.md`（U-00~U-05、D-01~D-31、R-01~R-08）；已知问题见
`TROUBLESHOOTING.md`（T-01 RPMsg 长跑退化——M33 侧根因待闭环）。

- **阶段1 wire 层**（`src/wire/`）：CRC16-CCITT-FALSE、帧编解码、流式分帧、41 条功能
  注册表（文档"42条"为勘误 D-15）、类型化载荷解析/构造（含 NaN/Inf 消毒）。
- **阶段2 核心**（`src/core/`、`src/net/`）：IRovControl 接缝、9 位权威状态机、优先级
  命令队列、41 项全功能分发（Safe 联动/Stop-Move 锁存/Estop-Emergency/推进器翻译矩阵/
  BaseValueVH 原子性/断线不重放）、WinSock/POSIX 可移植单客户端 TCP 服务端。
- **阶段3 传感器**（`src/sensors/`、`src/util/`）：Dht11Reader（misc/IIO/auto，D-21/22）、
  Ina226Reader（hwmon 发现+重扫，D-23）、M33SensorReader（MPU 换算、DYP 窗口判定）、
  SensorService（独立线程、TTL、100Hz 汇总流、ISensorBridge 联动 D-24/25/26）。
- **阶段4 实机接入**：
  - `src/core/rov_control_adapter.hpp`：IRovControl→真机 RovControl 纯转发（板端编译）；
  - `src/main_gateway.cpp`：完整 main（RPMsg 打开→启动对齐→传感器服务→TCP 监听→
    出站冲刷/1Hz 状态/心跳超时监督循环→SIGTERM 优雅退出+best-effort stop）；
  - `tools/gateway_probe.cpp`：板端验证客户端（连接推送/汇总流/只读查询/Estop 延迟）。

**实机证据摘要**（2026-09-03，详见 DECISIONS R-01~R-05）：启动对齐 mask=0x0066 正确；
汇总流 201 帧/2s≈100Hz、45B；DHT11 实读 4930=49%RH/30℃；DYP 65533 正确判无效；
Estop 延迟 5.1~7.2ms（DYP/MPU 轮询并发中）；SIGTERM 干净退出且 M33 全局停止锁存保持；
Windows 主机直连 192.168.1.120:7000 字节级验证通过。

未实现（后续阶段）：配置文件 INI/systemd/开机自启动/CamStream 编排（阶段5）、
Windows Salacia_Terminal 全链路对接（阶段6）。

## 板端操作（当前部署）

**开机自启动已启用**（`rov_gateway.service`，enable 状态）。上电后自动完成：
vendor 固件服务 → preflight 换载 ROV 固件并启动 M33 → 等 RPMsg → self-test →
网关监听 0.0.0.0:7000。日志：`journalctl -u rov_gateway -f`。

```bash
# 服务管理
systemctl status rov_gateway          # 状态（active = 正常）
systemctl restart rov_gateway         # 重启网关（不重启 M33，锁存保持）
systemctl stop rov_gateway            # 停止（M33 stop 锁存保持）
journalctl -u rov_gateway -f          # 实时日志

# 验证
/home/root/gateway/build/gateway_probe 7000 20

# 配置（/etc/rov_gateway.ini，示例见仓库 config/rov_gateway.ini）
vi /etc/rov_gateway.ini && systemctl restart rov_gateway

# 视频推流（默认未启用；需摄像头接好时手动开启）
systemctl enable --now rov_camstream  # 目标地址见 [camstream] 节

# 源码更新部署
#   主机侧：修改后通过 scripts/board_ssh.py 上传，然后：
cmake --build /home/root/gateway/build -j2 && ctest --test-dir /home/root/gateway/build
systemctl restart rov_gateway
```

**T-01 自动恢复已部署（2026-09-04 用户批准）**：MPU 数据链路看门狗
（`[gateway] mpu_watchdog_timeout_ms`，默认 60s，0=禁用）在 M33 楔死时
自动退出网关并由 systemd 重启链恢复（`rov_m33_gate.sh` 重启 M33+自检落
stop 锁存）；MPU/DYP 轮询失败有限频 journal 日志。根因仍在 M33 侧
（见 `TROUBLESHOOTING.md` T-01）。2026-09-04 新增：INA226/DHT11 开机
自准备单元（rov-ina226/rov-dht11）。

注意：网关运行期间独占 `/dev/ttyRPMSG0`——不得并发 `cat`/`echo`/
`rov_self_test`/`rov_api_smoke`/第二个网关实例。

## 构建

### 板端（OpenSTLinux，正式路径）

```bash
cmake -S /home/root/gateway -B /home/root/gateway/build -DCMAKE_BUILD_TYPE=Release
cmake --build /home/root/gateway/build -j"$(nproc)"
ctest --test-dir /home/root/gateway/build --output-on-failure
```

### 主机（无 CMake 时的等价构建，Windows MinGW / Linux）

```bash
bash scripts/build_host.sh     # 产出 build_host/test_* 与 build_host/rov_gateway
bash scripts/run_tests_host.sh # 运行全部测试（带超时保护）+ --check 自检
```

主机构建脚本与 CMakeLists 编译同一组源文件；新增源文件时两处同步。
Windows 主机链接 `-lws2_32`，POSIX 使用 `MSG_NOSIGNAL` 并忽略 SIGPIPE。

## 目录

```text
Gateway_A35/
├── PHASE0_REVIEW.md          # 阶段0只读审查（全量映射表/歧义/文件清单）
├── DECISIONS.md              # 用户批复（U-00~U-05）、设计决策（D-01~D-27）与实机证据（R-01~R-05）
├── Drivers.md                # DHT11/INA226 驱动要求（用户提供的规格）
├── CMakeLists.txt            # 板端链接 vendor+adapter+完整 main；主机为骨架
├── src/
│   ├── wire/                 # 协议层（阶段1）
│   ├── core/                 # 状态机/调度/优先队列/M33接缝+真机适配（阶段2/4）
│   ├── net/                  # 可移植 socket + 单客户端 TCP 服务端（阶段2）
│   ├── sensors/              # DHT11/INA226/M33 采集 + SensorService（阶段3）
│   ├── util/                 # 分级日志（限频）
│   ├── main.cpp              # 主机骨架（--check 自检）
│   └── main_gateway.cpp      # 板端完整 main（阶段4）
├── tests/                    # 13 个主机套件（板端同源 ctest 12 项）
├── tools/gateway_probe.cpp   # 板端验证客户端
├── scripts/                  # 主机构建/测试 + board_ssh.py 板端运维
└── vendor/rov_control/       # RovControl API v1 库源码（原样复制，板端链接）
```

## 运行边界（红线）

- 网关是 A35 上唯一持有 `/dev/ttyRPMSG0` 的进程；运行期间禁止 `cat`/`echo`/
  第二个 RovControl 进程/并行的 `rov_self_test`。
- 不生成 M33 ASCII 命令、不直接读写 `/dev/ttyRPMSG0`，一切经 RovControl。
- 不修改 Windows 终端、M33 固件、wire 协议、OpenAMP/remoteproc 与设备树。
