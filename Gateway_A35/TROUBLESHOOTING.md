# rov_gateway 故障排查记录（TROUBLESHOOTING）

可复用故障与处置记录。按 VibePrompt §十三：可复用故障写入本文件。

## T-01 RPMsg 长时运行后 write ENOMEM / 查询超时（2026-09-03，未闭环）

### 现象
- 网关带周期性 RPMsg 轮询（`sensor mpu`/`sensor dyp`）运行约 30~60 分钟后：
  - 先出现部分命令客户端超时（`get servo all`/`sensor mpu`/`sensor all`/`sensor dyp`
    等**数据类查询**超时），而 `stop`/estop 始终 1~7ms 快速 ACK；
  - 网关退出后，任何新进程（含 vendor `rov_api_smoke`/`rov_self_test`）写
    `/dev/ttyRPMSG0` 立即 `write: Cannot allocate memory`（A35 侧 rpmsg tx 池耗尽）；
  - remoteproc stop/start 后部分恢复，但再次快速退化；整机断电重启后完全恢复。
- 轮询关闭（mpu/dyp period 设 600000ms）时网关+M33 全部探针项 PASS。

### 判定依据（实测）
- 绕开网关、纯 vendor 工具复现 ENOMEM → 非 TCP/网关代码层问题；
- 故障与轮询**速率弱相关、与运行时长强相关**（50Hz→约30分钟、5Hz→约60分钟）；
- `stop` 快、数据查询慢/超时的选择性症状指向 M33 侧主循环被拖慢或 vring 回收停滞
  （候选：I2C8/MPU 路径阻塞拉伸主循环；DYP 异步路径；M33 已知技术债
  "I2C4/DMA fatal Error_Handler"、"IMU 读失败行为待注入测试"）。

### 处置（A35 侧，已部署）
- `/etc/rov_gateway.ini` 轻量轮询默认：`mpu_period_ms=1000`（1Hz）、
  `dyp_period_ms=5000`（5s）；
- 查询去重（D-28）：`sensor mpu`/`sensor all` 不再叠加 bridge 额外读；
- systemd `StartLimitBurst=5/300s` 防 self-test 失败时重启循环锤击 M33。

### 恢复手段（按代价从低到高）
```bash
systemctl restart rov_gateway                 # 网关侧恢复（多数情况不够；
                                               # 自检失败时 m33_gate 会自动重启 M33）
/home/root/ROV_M33/lib/fw_cortex_m33.sh start # 手动重启 M33（清锁存！输出归零）
reboot                                         # 整机断电级恢复（实测有效）
```

### 自动恢复（已部署，2026-09-04 用户批准）
网关内置 MPU 数据链路看门狗（`[gateway] mpu_watchdog_timeout_ms`，默认 60s，0=禁用）：
持续失败超时 → 网关以退出码 90 退出 → systemd 重启链中 `rov_m33_gate.sh`
在自检失败时自动重启 M33 并重落 stop 锁存。实机演练闭环通过（DECISIONS R-09）。
MPU/DYP 轮询失败现以限频日志出现在 journal（`sensor: mpu failure xN`），
journal 安静即数据链路健康。
注意：M33 重启会清 stop 锁存（变相 move-enable，输出已归零）；恢复后应立即
`stop all` 重新落锁存。

### 待办（超出本工程修改边界，需 M33 侧或用户决策）
- [ ] M33 侧根因定位（主循环周期测量/vring 回收审计）——不改 M33 源码无法闭环；
- [ ] >1 小时长跑验证与阈值确认（本阶段仅验证了 ~10 分钟稳定）；
- [ ] 若确认时间型退化：考虑 M33 看门狗+重启后自动 `stop()` 落锁存的方案
      （涉及安全语义，需用户批准后实施）。

## T-02 systemd ExecStartPre 失败导致重启循环（已修复）

`rov_gateway.service` 的 ExecStartPre（self-test）失败会触发 `Restart=on-failure`
循环，每次重试都重新执行 preflight+self-test，在 M33 已退化时形成锤击。
修复：unit 增加 `StartLimitIntervalSec=300` + `StartLimitBurst=5`。

## T-03 板端 SSH 无 SFTP 子系统

dropbear 配置不含 sftp-server，paramiko `open_sftp()` 报 ENOENT。处置：
`scripts/board_ssh.py` 通过 `exec_command("cat > remote")` + stdin 流式传输。
另：Git Bash 会把 `/home/...` 参数改写为 `D:/Git/home/...`，调用时需
`MSYS2_ARG_CONV_EXCL="*"`。
