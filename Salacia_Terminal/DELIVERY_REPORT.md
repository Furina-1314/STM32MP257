# Salacia_Terminal 第二轮优化交付报告

> 日期：2026-09-02
> 依据：`docs/VibePrompt.md`（第二轮优化提示词）
> 执行记录：`vibeplan.md` §R.1-R.12（Phase 12-19，8 个提交）
> **证据等级：Windows 侧与 Mock A35 验证全部通过；A35 实机对接未进行。**

## 1. 是否修改了 M33 或 A35

**否。** 本任务只修改了 `Salacia_Terminal/`（Windows 侧）及其文档；
未触碰 A35、M33、设备树、板端配置或任何外部 SDK。Windows 与 M33 保持双盲
（不生成 M33 ASCII 命令、不感知 M33 SEQ、不访问 RPMsg、不依赖 M33 源码）。

## 2. 提交与修改文件

| Phase | Commit | 修改文件 |
|---|---|---|
| 12 计划文档 | aabb892 | vibeplan.md（§R 总设计）、docs/VibePrompt.md（入库） |
| 13 协议层 | 52ab9fa | WireConstants.h、FunctionRegistry.h/.cpp、TcpClient.h/.cpp、ControlViewModel.cpp、SafetyStateModel.cpp、CommandPageWidget.cpp、test_registry/tcp/phase6/controlvm |
| 14 状态模型 | c000183 | SafetyStateModel.h/.cpp（重写）、MainWindow.cpp、ControlViewModel.cpp、CommandPageWidget.cpp、ControlAreaWidget.cpp、mock_a35.h/.cpp、test_safetystate（重写）/test_phase6/test_controlvm |
| 15 控制 VM | d4374e7 | ControlViewModel.h/.cpp（重写）、MainWindow.cpp、AppConfig.cpp、test_controlvm、mock_a35.cpp |
| 16 UI | 91906c8 + 2467faf | SwitchButtonWidget.h/.cpp（新增）、ControlAreaWidget.h/.cpp（重写）、CommandPageWidget.h/.cpp、CMakeLists.txt |
| 17 视频 | fb2d035 + 6f3cb01 | VideoFrameHub.h（新增）、GStreamerPipeline.h/.cpp、VideoGLWidget.h/.cpp、CommandPageWidget.h/.cpp、MainWindow.cpp、AppConfig.h/.cpp、app_config.ini、test_videohub（新增）、CMakeLists×2 |
| 18 窗口 | 4d46715 | MainWindow.cpp、AlarmBarWidget.cpp、AppConfig.h/.cpp、app_config.ini、test_windowgui（新增）、tests/CMakeLists |
| 19 文档验收 | 本次 | docs/WINDOWS_A35_INTERFACE.md（v2 改版）、README.md、HANDOFF.md、vibeplan.md、DELIVERY_REPORT.md（本文件） |

## 3. 新增/调整的 funcId 与状态字段

**新增 funcId（9 个，集中 WireConstants::Func + FunctionRegistry 全表登记）：**

| funcId | 函数 | 载荷 | 优先级 |
|---|---|---|---|
| 0x0013 | MoveAll（解除全局停止锁存） | 空 | 2 |
| 0x0014/0x0015 | StopVertical / MoveVertical | 空 | 2 |
| 0x0016/0x0017 | StopHorizontal / MoveHorizontal | 空 | 2 |
| 0x0024/0x0025 | VerticalSynchronization On/Off | 空 | 5 |
| 0x0026/0x0027 | HorizontalSynchronization On/Off | 空 | 5 |
| 0x0051 | BaseValueVH（姿态稳定双基准） | 2×i16（垂直、水平） | 5 |
| 0x0104 | StateEventV2（权威状态事件） | u8 version=2 + u16 mask | 事件 |

**变更 funcId：** 0x0010 更名 StopAll（复用 ID，语义=六路推进器置零并停止锁存）；
0x0011 Emergency 改"仅推进器置零，不上浮"；0x0012 Estop 载荷 32B→**空**
（删除"10 舵机+6 推进器零值"设计，绝不携带舵机角度）；0x0050 BaseValue 弃用；
0x0102 StateEvent 转 legacy（位义冻结，兼容回退）。

**优先级：** `Estop=0 > Emergency=1 > Stop/Move=2 > 普通=5`；紧急队列按优先级
稳定排序插入、永不丢弃（回归实测 estop 插队 11ms，红线 ≤100ms）。

**StateEventV2 掩码（9 位）：** bit0 safe、bit1 attitudeStabilization、
bit2 globalStopped、bit3 verticalStopped、bit4 horizontalStopped、
bit5 verticalSynchronization、bit6 horizontalSynchronization、bit7 estop、
bit8 emergency；stopped 位=1 表示停止锁存（UI 使能开关显示取反）；
未知位/版本不符整帧拒绝。

## 4. 执行器 ID 映射

| 类别 | wireId | UI 编号 | 标签 | 编码校验 |
|---|---|---|---|---|
| 舵机×10 | 0–9 | 1–10 | 舵机1（CH0）…舵机10（CH9） | set/get/mid id∈[0,9]，广播 0xFF |
| 垂直推进器×4 | 10–13 | 1–4 | 垂直1（CH10）…垂直4（CH13） | set id∈[10,15]（无广播），stop 0xFF=all |
| 水平推进器×2 | 14–15 | 1–2 | 水平1（CH14）、水平2（CH15） | 同上 |

UI 索引/显示编号/wireId 三分离，映射助手集中 WireConstants
（servoWireId/verticalWireId/horizontalWireId/thrusterWireIdFromFlat 等）；
非法 ID 编码器拒绝发送；AppConfig `servo_count/thruster_count/id_base` 弃用
（存在即告警）。

## 5. Safe/姿态稳定联动结果

- GUI 更名"姿态稳定（Horizontal）"（Roll/Pitch 自动调平，不代表水平推进器）。
- SafeOn 仅发一条 0x0020；GUI 将 Safe 与姿态稳定同时进入 Pending 目标 ON；
  成功 ACK 双 ON；失败/超时/断线**双回退**；A35 原子性由待确认项约束。
- Safe ON 期间姿态稳定开关保持 ON+禁用+"Safe 模式要求姿态稳定开启"；
  SafeOff 只关 Safe，姿态稳定保持 ON 恢复可操作。
- Safe ON **不锁舵机、不锁推进器提交**（A35 限幅/拒绝按返回结果回退+提示）。
- 非法权威组合 `Safe=ON 且 姿态稳定=OFF`：锁定推进器 + Error 级告警
  （ExInfoBar 弹窗 + AlarmModel + 日志），A35 修正后自动解锁；Windows 不悄悄修正。
- 以上均有自动化用例（test_safetystate 5 项 + test_phase6 端到端）。

## 6. Stop/Move/Synchronization 状态机

**三级使能（PendingSwitchState 事务）：**
- 开关：推进器总使能（MoveAll/StopAll）、垂直推进使能、水平推进使能；
  ON=允许运动，OFF=停止锁存；主页+指令页实时同步（同一 SafetyStateModel）。
- 事务规则（7 开关一致）：点击立即显示目标+ProgressRing+禁重复点击；成功
  ACK 提交；NACK/超时回退（ExInfoBar+AlarmModel+日志含 funcId/SEQ/目标/原因）；
  断线全部回 Unknown（半选态"状态未知"，不显示为已关闭）；StateEventV2 先于
  ACK 以事件为准；成功 ACK 后事件相反→事件覆盖+协议不一致高等级告警。
- 权限：推进器可操作=连接+权威已知+总使能 ON+分组使能 ON+相关开关非 Pending；
  总使能非 ON 时分组使能开关置灰（保留权威显示，禁止发起分组 Move）；
  舵机权限仅要求 TCP 连接（与全部推进器模式解耦，任何模式不置灰舵机）。
- 锁存与不重放：组停止锁存清该组待发（状态同步+冲刷时二次校验双保险），
  重新使能只恢复未锁存组、**不重放旧目标**。

**布局状态机（纯逻辑，可测）：**
```
姿态稳定 ON  → 每组 1 条基准滑条（BaseValueVH 双组独立）
姿态稳定 OFF → 垂直：同步 ON=1 条同步滑条 / OFF=4 条独立（同步 Unknown 保守逐路）
               水平：同步 ON=1 条 / OFF=2 条独立
```
姿态稳定 ON 时同步开关保留权威显示、可切换，布局不变+提示文案；
所有滑条保留 目标/已发送/A35 确认 三态与限频/松手冲刷/断线不重放。

## 7. 视频复用实现（VideoFrameHub）

- 单一 GStreamerPipeline、单一 UDP 视频端口、一套解码流程（不变式成立）；
- 显示通道改为 VideoFrameHub：互斥单帧槽 + `shared_ptr<const VideoFrame>`
  快照——主页与指令页两个 VideoGLWidget 实例各自 33ms 节拍快照渲染，
  **不竞争 RingBuffer、零拷贝、覆盖旧帧不积压、GUI 永不阻塞**；
- AI 通道 RingBuffer 链路不动；页面切换不停止/重启/重建管线；
- 指令页左上小视频：独立实例、尺寸 `[ui] command_video_width/height`（默认
  320×180）、信箱保比例、检测框归一化坐标自动缩放、无画面黑底；
  关闭流程 closeEvent 逆序首步 `releaseVideoGl()`（防 ICD 退出崩溃）。

## 8. 窗口尺寸修复

- 删除菜单折叠（MainWindow）与告警栏展开/收起（AlarmBarWidget）中全部顶层
  `setGeometry` 调用——只允许内部布局变化（告警面板限高 `[alarms]
  panel_max_height=260` + 内容区压缩）；
- 验证（test_windowgui，真实组件宿主窗口）：普通窗口展开/收起前后尺寸不变；
  最大化态全程 `isMaximized()==true`；10 次混合操作无尺寸漂移。

## 9. 构建结果

| 配置 | 结果 |
|---|---|
| Debug-x64（Ninja/MSVC） | 成功，零编译器警告（仅 Qt 私有模块 CMake 提示，既有） |
| Release-x64（Ninja/MSVC） | 成功（162/162），零警告 |
| 冒烟 | Debug/Release 主程序启动正常（双视频视图/新布局），退出干净 |

## 10. 测试总数及结果

**11 套件 135 用例，Debug 与 Release 双配置全部通过（135/135 + 135/135）：**

| 套件 | 用例 | 覆盖 |
|---|---|---|
| appconfig | 10 | 配置校验/弃用键告警 |
| wire | 14 | 帧编解码/分帧/重同步 |
| registry | 13 | 42 函数表唯一性/ID 拓扑边界/StateEventV2 解码/Stop 空载荷 |
| sensor | 10 | 双源取优/SOC/DYP 分级 |
| alarm | 8 | 合并/容量/筛选 |
| safety | 23 | PendingSwitchState 事务全规则/Safe 联动/非法组合/权限矩阵/布局判定/legacy |
| controlvm | 17 | 三态/限频/分组 API/双基准/Stop-Move 请求/锁存不重放/权限 |
| videohub | 8 | 最新帧/覆盖丢旧/双读者零拷贝/复位/单调序号 |
| windowgui | 7 | 真实窗口：展开收起/折叠尺寸不变/最大化恒真/无漂移 |
| tcp | 15 | 链路/ACK/超时/重连/estop 空载荷/紧急队列次序（Estop>Emergency>StopAll） |
| phase6 | 10 | 100Hz 长时/断连风暴/权限矩阵端到端（V2）/性能（estop 插队 11ms） |

## 11. 尚未完成的 A35 实机确认项（证据等级声明）

**本报告全部协议结论均为"Windows 侧和 Mock A35 通过"，不构成实机对接完成。**
待 A35 团队确认（详见 `docs/WINDOWS_A35_INTERFACE.md` §10）：

1. 新增 funcId 最终数值与 ACK 行为（0x0013–0x0017、0x0024–0x0027、0x0051、0x0104）；
2. StateEventV2 掩码位定义与发送时机（连接建立/状态变化/周期推送）；
3. Stop/Move 锁存与 stopped 位的对应关系；
4. SafeOn 原子性（姿态稳定先开再确认 Safe）与非法组合的板端约束；
5. 帧格式/CRC/100Hz 字段/DYP 单位/SOC 曲线/心跳/错误码表/端口/超时语义
   （一轮遗留项继续有效）。
