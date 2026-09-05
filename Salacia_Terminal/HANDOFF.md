# Salacia_Terminal 交接文档（HANDOFF）

> 生成时间：2026-08-30（Phase 0-6）；更新：2026-09-02（**第二轮优化 Phase 12-19 全部完成**）
> 用途：完整上下文交接。剩余：A35 实机实现与对接（**协议已定稿为最终版**，按 `docs/WINDOWS_A35_INTERFACE.md` §10 实现要求执行）、SOC 曲线标定、版本元数据、push/PR（用户决定）。
> **协议定稿（2026-09-02）**：Windows↔A35 接口 v2 为最终版，以 Windows 现状实现为准；
> 文档不再含"待确认"协议项；后续扩展按接口文档 §0.5 演进规则（新函数用未占用 ID、
> 状态扩展递增 StateEventV2 version）。
> **最新状态（2026-09-02，第二轮优化）**：依据 `docs/VibePrompt.md` 完成 8 个阶段——
> ①协议层扩展（新 funcId 0x0013-17/0x0024-27/0x0051/StateEventV2 0x0104、执行器 ID 拓扑
> 舵机 wire0-9/垂直 10-13/水平 14-15、Estop 空载荷删除 32B 舵机设计、
> 优先级 Estop>Emergency>StopMove>普通 且紧急队列稳定排序）；
> ②SafetyStateModel 重写（PendingSwitchState×7、Safe↔姿态稳定联动、非法组合
> Safe=ON+Stab=OFF 锁推进器+高等级告警、舵机权限与全部模式解耦、布局判定纯逻辑）；
> ③ControlViewModel 分组重构（垂直/水平分组、双基准 BaseValueVH、Stop/Move 请求、
> 锁存清待发+重新使能不重放）；④UI（主页分组推进器+布局动态切换+使能/同步开关、
> 指令页 7 模式开关同一模型+控制区网格重排 5×2/2×2/2×1 水平滑条+开关联动置灰）；
> ⑤指令页小视频（VideoFrameHub 最新帧共享层，单管线单端口，双视图零拷贝）；
> ⑥窗口尺寸修复（删除全部顶层 setGeometry，只改内部布局）+GUI 测试；
> ⑦文档改版（接口文档 v2/README/本文件/DELIVERY_REPORT.md）；
> ⑧Debug+Release 双构建与 11 套件 135 用例全绿（证据等级：**Windows 侧与
> Mock A35 通过，A35 实机未对接**）。
> 权威计划文件：`vibeplan.md`（一轮 Phase 0-11 + 二轮 Phase 12-19 全记录）
> 需求原始文件：`docs/VibePrompt.md`（二轮）+ `Salacia_Terminal_优化开发提示词.md`（一轮）
> + `ROV_A35_M33_Control_Protocol_v1.0.md`（仅业务语义参考）

---

## 一、项目与环境

| 项 | 值 |
|---|---|
| 项目 | Windows ROV 岸上终端 `Salacia_Terminal`（Qt Widgets，仅 Windows） |
| 路径 | `E:\STM32MP257\Code\STM32MP257\Salacia_Terminal` |
| 仓库 | https://github.com/Furina-1314/Salacia.git，分支 **ZCode**（PR #2 开放中） |
| 编译器 | VS2026 Insiders MSVC 14.51 @ `F:/Microsoft Visual Studio/18/Insiders` |
| Qt | 6.11.1 msvc2022_64 @ `F:/Qt/6.11.1/msvc2022_64`（Test/Svg/Quick3D/CorePrivate 等已接入） |
| GStreamer | 1.28.6 @ `F:/gstreamer/1.0/msvc_x86_64`（UDP 视频接收，**行为不动**） |
| ONNX Runtime | directml-1.24.4 @ `F:/onnxruntime`（POST_BUILD 拷 DLL 防 System32 遮挡） |
| libssh | `F:/libssh-0.12.2-msvc`（**Phase 5 删除**） |
| 构建 | CMake Presets `Debug-x64`/`Release-x64` → `out/build/{debug,release}`，Ninja |
| 实网配置 | host 192.168.1.100 / board 192.168.1.120（用户路由器环境，勿改回 137.x） |
| AI 模型 | `models/model.onnx`（7 类水下目标），标签 `E:/STM32MP257/AIModel/data.yaml` |

### 构建命令（必须走 vcvars，直接 ninja 会缺 INCLUDE）
```
# Debug（临时 bat 已存在 C:\Users\hzxfl\AppData\Local\Temp\salacia_build_debug.bat）
call "F:\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
set QTDIR=F:/Qt/6.11.1/msvc2022_64
cmake --build out\build\debug      # 或 out\build\release
```

### 测试命令（11 套件 135 用例，全部通过中）
```
cd out\build\debug
.\salacia_tests_appconfig.exe / .salacia_tests_wire.exe / .salacia_tests_registry.exe /
.\salacia_tests_sensor.exe / .salacia_tests_tcp.exe / .salacia_tests_alarm.exe /
.\salacia_tests_safety.exe / .salacia_tests_controlvm.exe / .salacia_tests_phase6.exe /
.\salacia_tests_videohub.exe / .salacia_tests_windowgui.exe（真实窗口 GUI）
（exe 为 WIN32 子系统无控制台输出，用 -o 文件,txt 或 ctest）
```

---

## 二、纪律红线（违反即返工）

1. **一次一个 Phase，Phase 完成后停止**，输出 §十五 格式报告，等用户"编译通过/继续"。
2. **不擅自 git commit/push/PR**（提示词 §4.3）。当前工作区有 Phase 1-4 全部未提交变更（大量修改 + tests/ + src/ui/ + vibeplan.md + 两个需求文档未跟踪）——由用户决定提交时机。
3. **字符集**：C++ 源码 GBK 无 BOM；ini/QML/Markdown UTF-8；中文字符串一律 `QString::fromLocal8Bit`（**禁止 QStringLiteral/QStringLiteral 宏装中文**，clang 会报 illegal character encoding）；新写文件后必须用 Python 校验/转换 GBK。编辑器曾把 GBK 存成 UTF-8+BOM 造成事故。
4. **范围**：只改 Windows 侧；不动 A35/M33/板端代码；不改 UDP 视频行为；TCP wire 已定稿为最终版（2026-09-02，`docs/WINDOWS_A35_INTERFACE.md` v2，以 Windows 现状实现为准，字段不再变更）；实机对接完成前不得宣称"实机通过"（Mock A35 通过 ≠ 实机通过）。
5. **线程**：Worker-Object（QObject+moveToThread，禁重写 run）；跨线程 UI 显式 QueuedConnection；GUI 零阻塞；退出逆序 视频→遥测→TCP→(SSH)→AI→Logger，`ExitProcess` 前 `exit_grace_ms` 静默。
6. **TD-8**：退出时不销毁 QOpenGLWidget/GstPipeline（故意泄漏防 Intel ICD 崩溃）；每 Phase 冒烟须验证优雅关闭退出码 0x0。
7. **不用 0 冒充无效传感器值**；模式/状态以 A35 ACK/StateEvent 为准（本地点击只算"请求中"）。
8. 配置唯一源 `config/app_config.ini`；默认值必须写在 ini 里，不藏代码。

---

## 三、环境坑（已踩平，新会话勿再踩）

| 坑 | 规避 |
|---|---|
| Qt 6.11.1 qrunnable.h:26 是 `const QRunnable()`（预览包缺陷），`<QtTest>` 伞头 + `/permissive-` 报 C7731 | 测试用窄化包含 `#include <QtTest/qtest.h>` |
| QSettings IniFormat 把含逗号的值解析成 QStringList（toString 得空串） | soc_curve 用空格分隔 `13.0 14.0 : 0 20`；读取侧已做列表 join 兼容 |
| UTF-8 中文注释在 GBK 翻译单元会吞行尾破坏下一行代码（实验证实） | **全库统一 GBK**：src/ui 第三方 230 文件已转 GBK，中文字面量改 fromLocal8Bit；qwindowkit 已剥 `/utf-8` 注入并转 GBK |
| Qt 目标注入 `/utf-8` 与 `/source-charset:GBK` 冲突 D8016 | tests/ 与需要处设 `QT_NO_UTF8_SOURCE TRUE`（src/ui 现为 GBK 不再需要 per-source 标志） |
| clang-tidy 对 390KB fluentui3style.cpp 与 qwindowkit OOM | `src/ui/CMakeLists.txt` 与 `tests/` 目录作用域 `set(CMAKE_CXX_CLANG_TIDY "")` |
| `qt_standard_project_setup` 不开 AUTORCC | ui 目标显式 `AUTORCC ON`（resource.qrc 含 Segoe Fluent Icons） |
| 样式库文件名是 `fluentuiappearance.*`（无"3"） | — |
| 每用例复用 QTcpServer 监听失败 | mock 在各测试用例内局部实例化 |
| QtTest 三个 QTEST_MAIN 链接冲突（LNK1169 多 main） | 拆独立测试可执行目标 |
| 主程序 exe 被运行中实例锁（LNK1168） | 构建前 taskkill（注意用户可能在测试，先确认） |

---

## 四、当前架构（2026-09-02，二轮优化后）

```
main.cpp（FluentUI3Style+浅色+Fluent 配色应用，崩溃转储，TD-8 退出）
└─ MainWindow（FluentWindowFrame 无边框 + ExWinUINavigationView 导航，
   折叠/展开与告警栏展开只改内部布局，窗口尺寸/最大化状态不变）
   ├─ 主菜单：主页、指令（页脚：设置、关于）
   ├─ 主页：视频(VideoGLWidget)｜姿态(QQuick3D RovViz.qml)+传感器卡
   │        ／ 控制区（舵机 10 路竖直滑条[舵机1（CH0）..] +
   │          垂直推进器组 CH10-13 / 水平推进器组 CH14-15[各带同步开关，
   │          独立/同步/基准三页布局切换] + 推进器使能列三级开关 + 紧急停机固定区）
   ├─ 指令页：左上小视频（与主页共享 VideoFrameHub 单管线）+ 7 模式事务开关
   │          （与主页同一 SafetyStateModel）+ 控制区（舵机 5×2/垂直 2×2/水平 2×1
   │          网格，两行式水平滑条，使能开关底部单行）+ 查询表单 + 结果表
   ├─ 顶部告警摘要条（可展开限高 [alarms] panel_max_height）+ 底部状态栏（视频/AI/遥测/TCP）
   └─ 模型层：
      SafetyStateModel（PendingSwitchState×7 事务[目标显示/Ring/回退/冲突告警]、
        Safe↔姿态稳定单向联动、非法组合 Safe=ON+Stab=OFF 锁推进器、
        舵机权限与全部模式解耦、分组权限、布局判定 Individual/Sync/Base）
      ControlViewModel（分组通道 CH10-13/14-15 + wire 映射、双基准 BaseValueVH、
        Stop/Move 三级请求、锁存清待发+重新使能不重放、三态/限频/松手冲刷）
      AlarmModel（三级/合并/容量/Error 置顶）
      SensorModel（TCP100Hz+UDP5001 双源取优、SOC 曲线/待标定、DYP 六态）
```

通信面（协议 v2 最终版，42 函数）：`TcpClient`（Worker，双优先级队列[紧急通道按
优先级 Estop>Emergency>StopMove 稳定排序、永不丢弃]、指数退避重连、seq-ACK 匹配、
迟到丢弃、断开清队不重放、重连后 ask/status/sensor all 权威查询、心跳）+
`WireCodec/FrameAccumulator`（分帧/重同步）+ `FunctionRegistry`（42 条目 + 强类型
编解码；执行器 ID 拓扑：舵机 wire 0-9 / 垂直 10-13 / 水平 14-15，UI/wire 三分离）+
StateEventV2（0x0104）权威状态 + `VideoFrameHub`（显示帧最新帧发布层，双视图
零拷贝）+ `UdpReceiver`（遥测回退，ini 开关）。
（一轮 Phase 4 末架构与 SSH 时代的历史记录见 vibeplan.md 各 Phase 交付记录。）

---

## 五、未完成需求（按 vibeplan Phase 5/6 执行）

### Phase 5 已完成（2026-08-30）
- **C5.1 指令页**：参数化表单覆盖注册表全部已确认函数（ask/ver/status/help、stop/emergency/estop、safe·horizontal on/off、servo set·get·mid·all、propeller set·get·base·real·stop·all、sensor mpu·dyp·all）；每条显示 Sequence/ACK/耗时/错误；高级原始命令入口仅限注册表已知函数（禁 shell）；A35 未确认的命令标记不可用。
- **C5.2 设置页 + 关于页**：设置页=主题/配色即时切换（FluentUIAppearance/`_q_themestyle`，含标题栏 setThemeDark）、关键运行参数摘要（改 ini 重启生效提示）、"打开配置目录"按钮；关于页=软件名/版本/作者/协议版本占位（不编造，列待提供项）。
- **C5.3 控制区完整交互**：滑条上方当前设定值、下方输入框（空值/非法字符/小数/越界/粘贴/滚轮/失焦处理）；区分目标值/已发送值/A35 确认值（三态已有，补输入框路径）。
- **C5.4 删除 SSH 运行时**：删 `src/communication/SshClient.*`；MainWindow 清 include/成员/closeEvent 的 sshClient_->stop()/sshLabel_（状态栏改四组）；CMake 移除 libssh 发现块（约 135-164 行）与 `SALACIA_SSH_LINK` 链接（约 228-230 行）；ini 删 `[rov] ssh_host/ssh_port/ssh_user/ssh_password/ssh_key_path/ssh_reconnect_sec`（写迁移说明）；删 `src/widgets/ControlPanelWidget.*`（已不被引用）；全库 grep ssh/libssh 零残留（文档除外）。**注意**：AppConfig 的 sshXxx() getter 同步删除，引用处（日志摘要）清理。
- **C5.5 文档**：README 更新（新架构/配置/测试/FluentUIStyle 复制说明）、Windows↔A35 接口说明（含函数表与错误映射）、SSH 移除清单、迁移说明。

### Phase 6 已完成（2026-08-30）：280s 100Hz 零丢帧；estop 插队 11-19ms；出队 8-28ms；105/105 用例
- **C6.1** 全量 mock 回归：100Hz 长时（≥5min）、断连风暴、错误码→告警映射、权限矩阵全状态遍历（可扩 mock：按 StateEvent 场景脚本驱动）。
- **C6.2** 性能测量记录：100Hz 接收 CPU/内存、滑条→报文出队延迟、estop 插队延迟（正常队列积压时）。
- **C6.3** Debug+Release 双构建零新告警、交付物清单核对（提示词 §十六）、待确认项终版；可选：不同 DPI/窗口尺寸走查。

---

## 七、新对话提示词（复制整段即可）

```
你是资深 Windows C++/Qt 工程师，继续开发 ROV 岸上终端 Salacia_Terminal。项目位于
E:\STM32MP257\Code\STM32MP257\Salacia_Terminal（分支 ZCode，一轮 Phase 0-11 与
二轮 Phase 12-19 均已完成并提交）。

【第一步：只读上下文，禁止先改代码】依次完整阅读：
1. Salacia_Terminal/HANDOFF.md（交接文档：环境、纪律红线、已踩平的环境坑、当前架构）
2. Salacia_Terminal/docs/WINDOWS_A35_INTERFACE.md（**协议 v2 最终版**：全部对接以此为准）
3. Salacia_Terminal/vibeplan.md（权威计划：一轮 Phase 0-11 + 二轮 Phase 12-19 全记录）
4. Salacia_Terminal/docs/VibePrompt.md（二轮需求）+ ROV_A35_M33_Control_Protocol_v1.0.md（仅业务语义）
然后运行 Debug 构建（VsDevCmd -arch=x64 后 cmake --build out\build\debug）与全部测试
（out\build\debug 下 11 个 salacia_tests_*.exe，135 用例应全绿；exe 为 WIN32 子系统，
用 -o 文件,txt 方式运行）确认基线，再汇报"基线确认"。

【纪律（每条都是硬约束）】
- TCP wire 协议已定稿（v2 最终版，2026-09-02）：funcId/载荷/位义不再变更；扩展按接口
  文档 §0.5 演进规则（新函数用未占用 ID、状态扩展递增 StateEventV2 version），
  变更须双方确认并同步文档与 WireConstants。
- 禁止：修改任何 A35/M33/板端代码；宣称"实机通过"（当前证据等级 = Windows 侧 +
  Mock A35）；改变 UDP 视频行为；提供 shell 入口；裸 struct 直接发送；GUI 线程阻塞
  I/O；用 0 冒充无效传感器值；把本地点击当作板端成功。
- 字符集：C++ 源码必须 GBK 无 BOM（新写文件后用 Python 校验/转换）；ini/QML/Markdown
  UTF-8；中文字符串一律 QString::fromLocal8Bit，禁止 QStringLiteral 装中文。
  注意：GBK 源文件里 grep 中文匹配不到，需用 Python 检索。
- 线程：Worker-Object 模式（QObject+moveToThread）；跨线程 UI 显式 Qt::QueuedConnection；
  退出逆序 双视频 releaseGl → 视频管线 stopForExit → 遥测 → TCP → AI → Logger；
  保留 TD-8 规避（ExitProcess 前 exit_grace_ms 静默期）。
- 配置唯一源 config/app_config.ini，默认值写在 ini 不藏代码；缺关键键禁用对应功能并告警。
- 环境坑清单见 HANDOFF.md 第三节（qrunnable 窄化包含、QSettings 逗号、GBK 统一、
  clang-tidy OOM、AUTORCC、LNK1168 exe 文件锁等）。
- 每次交付后：Debug+Release 双构建零告警、全部测试绿、主程序冒烟含优雅关闭。

【当前状态摘要】二轮优化（Phase 12-19）已完成：协议层 42 函数（StateEventV2 0x0104、
BaseValueVH 0x0051、Stop-Move 分组 0x0013-17、双同步 0x0024-27、Estop 空载荷、
优先级 Estop>Emergency>StopMove>普通）、SafetyStateModel 重写（PendingSwitchState×7、
Safe↔姿态稳定联动、非法组合锁推进器、舵机权限解耦）、ControlViewModel 分组重构
（CH10-13/14-15、双基准、锁存不重放）、主页分组布局+指令页 7 模式开关+控制区网格重排
（舵机 5×2/垂直 2×2/水平 2×1 水平滑条）、VideoFrameHub 双视图共享小视频、
窗口尺寸不变修复；11 套件 135 用例 Debug/Release 双配置全绿。
剩余：A35 实机实现与对接（按接口文档 §10 实现要求）、SOC 曲线标定、push/PR（用户决定）。

现在开始执行第一步（只读+基线确认），然后停下等我确认。
```
