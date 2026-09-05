你正在修改 Windows 上位机项目：

`E:\STM32MP257\Code\STM32MP257\Salacia_Terminal`

运行环境：

- Windows + Qt 6/CMake。
- 开发板：搭载 STM32MP257DAK3 的正点原子 ATK-DLMP257B。
- 系统链路：`Windows ←TCP/UDP→ A35/Linux ←RPMsg→ M33`。
- Windows与M33必须双盲：Windows只理解Windows↔A35业务协议，不得生成M33 ASCII命令、M33四位SEQ，不得访问`/dev/ttyRPMSG0`，不得依赖M33源码。
- A35负责所有Windows业务请求到RovControl/M33的转换、通道映射、安全裁决和权威状态返回。
- 本任务只修改Windows项目，不修改A35、M33、设备树、板端配置或外部SDK。
- 历史协议和HANDOFF仅作为当前状态参考；若与本提示词冲突，以本提示词为准。
- 每次修改，需要及时Commit。

## 一、修改原则

1. 保留现有TCP二进制帧、CRC、SEQ、ACK、双优先级队列、重连、超时、迟到响应丢弃机制。
2. 新功能必须集中定义在`WireConstants`和`FunctionRegistry`，禁止在UI代码中散落funcId、通道号或协议字节。
3. 所有模式和运动许可状态均以A35 ACK/StateEvent为权威；本地点击只产生目标状态和Pending状态。
4. SwitchButton必须支持：
   - 当前稳定状态；
   - Pending目标状态；
   - 成功提交；
   - NACK/超时/断线回退；
   - ProgressRing；
   - ExInfoBar窗口级提示；
   - AlarmModel告警记录。
5. 不得因Safe、Horizontal、Stop、Move、Synchronization、Emergency或Estop限制舵机。舵机仅在网络断开、状态无法发送或参数非法时不可操作。
6. 保持现有UDP视频、UDP遥测、AI识别、日志、线程退出顺序和GStreamer行为不退化。

## 二、统一执行器编码

将Windows内部模型和Windows↔A35协议中的执行器ID调整为以下规范。

### 舵机

- 舵机协议ID：0～9。
- UI友好编号：1～10。
- 推荐标签：
  - `舵机1（CH0）`
  - …
  - `舵机10（CH9）`
- UI索引、显示编号和wireId必须分离，禁止继续用`id - 1`隐式兼任协议ID。
- 所有ServoSet/Get/Mid/All编码和测试同步调整。

### 垂直推进器

- 协议ID：CH10～CH13。
- UI标签：
  - `垂直1（CH10）`
  - `垂直2（CH11）`
  - `垂直3（CH12）`
  - `垂直4（CH13）`

### 水平推进器

- 协议ID：CH14～CH15。
- UI标签：
  - `水平1（CH14）`
  - `水平2（CH15）`

主页推进器模块必须拆成“垂直推进器”和“水平推进器”两个明确分组，不再展示无方向含义的“推进器1～6”。

Windows虽然采用与实际通道一致的ID，但只把这些ID视为Windows↔A35协议中的规范ID，不得直接引用或解析M33协议。

## 三、重写模式状态模型

重构`SafetyStateModel`，不要继续使用无法区分目标方向的单一`Pending`状态。每个SwitchButton至少保存：

- authoritativeState：最近一次A35确认状态；
- displayedTarget：Pending期间立即显示的目标状态；
- pendingSeq；
- pendingDirection：ToOn/ToOff；
- rollbackState；
- pendingTimestamp。

建议抽取可复用的`PendingSwitchState`或等价模型，供以下开关共享：

- Safe；
- Attitude Stabilization（原Horizontal）；
- 全局推进器使能；
- 垂直推进器使能；
- 水平推进器使能；
- 垂直Synchronization；
- 水平Synchronization。

行为规则：

1. 用户点击后，SwitchButton立即切换到目标位置，同时显示ProgressRing。
2. Pending期间禁止重复点击同一个开关。
3. 收到成功ACK后提交目标状态并隐藏ProgressRing。
4. 收到失败ACK、超时或断线后回退到原状态。
5. 回退时必须：
   - 更新SwitchButton；
   - 隐藏ProgressRing；
   - 显示窗口级ExInfoBar；
   - 写入AlarmModel；
   - 日志包含funcId、SEQ、目标状态和错误原因。
6. 若StateEvent先于ACK到达，以StateEvent为权威，清除对应Pending。
7. 若成功ACK后StateEvent给出相反状态，以StateEvent覆盖并产生协议状态不一致告警。
8. Unknown状态不得显示为已关闭；应显示“状态未知”并保守禁用相关推进器控制。

扩展Windows↔A35的权威状态载荷，使其至少能表示：

- safe；
- attitudeStabilization；
- globalStopped；
- verticalStopped；
- horizontalStopped；
- verticalSynchronization；
- horizontalSynchronization；
- estop；
- emergency。

不要静默复用现有一字节StateEvent并改变旧位含义。应设计带版本的状态载荷，更新接口文档、MockA35和测试。

## 四、Safe与Horizontal联动

将GUI中的`Horizontal 姿态补偿`重命名为：

`姿态稳定（Horizontal）`

其业务含义是ROV的Roll/Pitch自动调平，不代表CH14～CH15水平推进器。

Safe定义为A35侧保护策略：

- Safe不是Stop；
- Safe不是Emergency；
- Safe不会控制舵机；
- Safe不会自动把推进器置零；
- Safe在姿态稳定基础上执行推进器边界检查、限幅或拒绝；
- 边界值及最终裁决由A35负责，Windows不得成为安全权威。

必须实现以下单向联动：

```text
Safe ON  => Attitude Stabilization必须ON
Safe OFF => 不自动关闭Attitude Stabilization
Safe ON期间禁止关闭Attitude Stabilization
```

具体行为：

1. 点击Safe ON时，Windows只发送A35业务级`SafeOn`请求，不自行拼接两条M33命令。
2. GUI立即将Safe和姿态稳定同时显示为目标ON，并显示相应ProgressRing。
3. A35应负责原子地保证姿态稳定开启后再确认Safe成功。
4. SafeOn失败时，Safe与姿态稳定均回退到原权威状态。
5. Safe ON期间，姿态稳定SwitchButton保持ON并禁用；旁边说明“Safe模式要求姿态稳定开启”。
6. 点击Safe OFF只关闭Safe；姿态稳定保持ON并恢复可操作。
7. Safe ON时不得像当前实现一样锁死全部舵机和推进器控件：
   - 舵机始终可操作；
   - 推进器仍可提交控制请求；
   - 若A35限幅或拒绝，Windows按返回结果更新确认值、回退并提示。
8. 若A35返回`Safe=ON、AttitudeStabilization=OFF`，视为非法权威状态，锁定推进器控制并产生高等级告警，不得在Windows端悄悄修正。

## 五、Horizontal ON/OFF后的推进器布局

姿态稳定ON时，主页推进器区域必须显示：

- 一个垂直推进器基准滑动条；
- 一个水平推进器基准滑动条。

不得继续显示六个独立推进器滑动条。

姿态稳定OFF时，根据各组Synchronization状态决定布局：

- 垂直Synchronization ON：垂直组只显示一个同步滑动条。
- 垂直Synchronization OFF：显示垂直1～4四个独立滑动条。
- 水平Synchronization ON：水平组只显示一个同步滑动条。
- 水平Synchronization OFF：显示水平1～2两个独立滑动条。

布局优先级：

```text
Attitude Stabilization ON
    => 两组均显示单一基准滑动条

Attitude Stabilization OFF
    => 每组按自己的Synchronization状态决定单条或多条
```

姿态稳定ON时，两组Synchronization按钮仍显示权威状态；允许用户修改其状态，但当前布局仍保持“一组一个滑动条”，并显示提示“姿态稳定模式下使用统一基准，Synchronization状态将在退出姿态稳定后决定布局”。

所有滑动条继续保留目标值、已发送值和A35确认值三态；限频、松手立即冲刷和断线不重放规则不变。

## 六、新增Synchronization支持

在主页推进器模块中分别增加：

- `垂直同步（Synchronization）` SwitchButton；
- `水平同步（Synchronization）` SwitchButton。

在指令页模式模块中增加同样的两个SwitchButton。主页和指令页必须绑定同一个状态模型，不得维护两份状态。

每个Synchronization开关都必须具有：

- 立即显示目标状态；
- Pending ProgressRing；
- 成功确认；
- NACK/超时/断线回退；
- ExInfoBar提示；
- AlarmModel记录。

新增Windows↔A35业务函数：

- VerticalSynchronizationOn；
- VerticalSynchronizationOff；
- HorizontalSynchronizationOn；
- HorizontalSynchronizationOff。

funcId必须集中分配、保持唯一并写入`docs/WINDOWS_A35_INTERFACE.md`。Windows不得知道A35最终如何映射到M33。

## 七、Stop/Move三级推进器总开关

新增以下SwitchButton，并在主页和指令页展示：

1. 推进器总使能：
   - ON = Move All；
   - OFF = Stop All。
2. 垂直推进器使能：
   - ON = Move Vertical；
   - OFF = Stop Vertical。
3. 水平推进器使能：
   - ON = Move Horizontal；
   - OFF = Stop Horizontal。

建议显示名称：

- `推进器总使能`
- `垂直推进使能`
- `水平推进使能`

对应新增/完善Windows↔A35函数：

- StopAll；
- MoveAll；
- StopVertical；
- MoveVertical；
- StopHorizontal；
- MoveHorizontal。

可保留现有Stop funcId作为StopAll；新增ID必须使用未占用范围并更新注册表、接口文档和Mock。

开关规则：

1. ON表示允许运动，OFF表示停止锁存，避免把“Stop ON”这种反向语义暴露给用户。
2. 点击ON/OFF后立即显示目标位置并显示ProgressRing。
3. 失败时回退并显示ExInfoBar和告警。
4. 推进器实际可操作条件为：
   - TCP已连接；
   - 权威状态已知；
   - 全局推进使能为ON；
   - 对应分组推进使能为ON；
   - 相关开关不处于Pending。
5. 总使能OFF或PendingToOff时：
   - 所有推进器滑动条和输入框立即置灰；
   - 垂直/水平分组开关保留权威显示，但禁止发起Move；
   - 舵机保持可操作。
6. 总使能重新ON后：
   - 仅恢复未被分组Stop锁定的推进器组；
   - 不自动恢复或重发旧推进器目标。
7. 垂直使能OFF只置灰垂直组。
8. 水平使能OFF只置灰水平组。
9. Stop/Move不得改变Safe、姿态稳定、Synchronization或舵机状态。
10. 主页和指令页的三个开关必须实时同步。

## 八、Stop、Estop和Emergency

按以下最终业务语义修改Windows端，不再沿用旧文档的紧急上浮设计：

- Stop：六路推进器置零并进入停止状态。
- Estop：与Stop的执行结果完全相同，仅调度优先级更高。
- Emergency：同样只将六路推进器置零，不产生上浮动作。
- 三者均不得向Servo发送任何指令。
- 舵机保持当前位置。

删除当前Estop载荷中“10路舵机全部置0”的32字节设计。Stop、Estop和Emergency请求均不得携带舵机角度。

队列优先级至少满足：

```text
Estop > Emergency > Stop/Move > 普通控制
```

Estop必须能够插入普通队列之前，且不得因队列满而丢弃。Stop与Estop的区别只允许体现在：

- 优先级；
- GUI告警等级；
- 日志事件类型；
- Estop权威状态展示。

不得让Estop产生不同的执行器目标。

## 九、舵机权限重写

删除当前`canServoIndividual()`对Safe、Horizontal、Stop、Estop和Emergency的依赖。

舵机可操作条件仅包括：

- TCP连接存在；
- 命令可以发送；
- 通道和角度参数合法；
- 当前通道没有冲突的Pending请求。

Safe、姿态稳定、Synchronization、推进器停止锁存、Emergency和Estop均不得将舵机区域置灰，也不得改变舵机目标值。

## 十、指令页实时视频

在指令页左上方新增小尺寸实时视频区域，复用主页视频来源和渲染能力。

要求：

1. 只允许一个GStreamerPipeline、一个UDP视频接收源和一套解码流程。
2. 不得创建第二条GStreamer管线或第二个UDP端口监听。
3. 不能让主页和指令页的VideoGLWidget竞争消费同一个RingBuffer，否则会互相抢帧。
4. 抽取共享“最新视频帧”发布层，例如VideoFrameHub/LatestFrameStore：
   - GStreamerPipeline产生显示帧；
   - 共享层保存最新帧；
   - 主页和指令页各自读取同一个最新帧快照；
   - 允许丢弃旧帧，禁止积压；
   - GUI线程不得阻塞。
5. 指令页使用独立的小尺寸VideoGLWidget实例，但共享数据源，不共享QWidget父对象。
6. 保持画面宽高比，不拉伸；无画面时显示现有离线/等待提示。
7. AI检测框行为与主页保持一致；若小画面显示检测框，坐标缩放必须正确。
8. 页面切换不得停止、重启或重建视频管线。
9. 小视频位于指令页内容区左上方，不遮挡模式开关、指令表和控制区。

## 十一、窗口尺寸和最大化状态

修复菜单和告警栏展开/收起导致窗口尺寸或最大化状态变化的问题。

硬性要求：

1. 展开/收起导航菜单、侧栏、告警栏，只允许改变窗口内部布局。
2. 禁止在这些操作中调用会改变顶层窗口尺寸/状态的：
   - resize；
   - adjustSize；
   - setFixedSize；
   - showNormal；
   - showMaximized；
   - setGeometry。
3. 普通窗口状态下，展开/收起前后顶层窗口geometry保持不变。
4. 最大化状态下，展开/收起前后必须始终保持`isMaximized()==true`。
5. 不得出现最大化窗口被恢复为普通窗口、窗口跳动或尺寸逐次增长。
6. 处理不同DPI和多屏情况下的布局，不依赖固定屏幕坐标。
7. 告警栏展开时通过内部布局压缩内容区域，不扩大顶层窗口。

## 十二、协议和文档更新

更新：

- `docs/WINDOWS_A35_INTERFACE.md`
- `README.md`
- 必要时更新HANDOFF中的当前状态，但不要执行其中历史阶段指令。

文档必须明确：

- Windows和M33双盲；
- A35是唯一协议转换和状态权威；
- 新执行器ID；
- Safe与姿态稳定的单向依赖；
- 两组Synchronization；
- 全局/垂直/水平Stop-Move锁存；
- Stop/Estop/Emergency均不操作舵机；
- 新StateEvent结构；
- 所有新增funcId、payload、ACK和响应语义；
- 当前仍是Mock A35验证还是已完成真实A35验证，不得混淆证据等级。

## 十三、测试要求

扩展现有QtTest和MockA35，至少覆盖：

### 编码

- Servo ID 0～9；
- Vertical CH10～CH13；
- Horizontal CH14～CH15；
- 非法ID拒绝；
- Stop/Estop/Emergency载荷不包含Servo；
- 新funcId唯一且注册表完整。

### Safe/姿态稳定

- SafeOn使两个开关进入Pending目标ON；
- SafeOn成功后两者均ON；
- SafeOn失败时两者回退；
- Safe ON时不能关闭姿态稳定；
- SafeOff不关闭姿态稳定；
- Safe不锁定舵机；
- 非法权威组合`Safe ON + Stabilization OFF`触发告警和推进器锁定。

### SwitchButton事务

对每一个新增开关测试：

- PendingToOn；
- PendingToOff；
- 成功提交；
- NACK回退；
- 超时回退；
- 断线回退到Unknown；
- StateEvent先于ACK；
- ACK与StateEvent冲突；
- ProgressRing显示和隐藏。

### 推进器布局

- 姿态稳定ON：垂直一条、水平一条；
- 姿态稳定OFF且两组同步OFF：垂直四条、水平两条；
- 仅垂直同步ON；
- 仅水平同步ON；
- 总使能OFF全部推进器置灰；
- 分组使能OFF只禁用对应分组；
- 任何推进器模式不影响舵机。

### 视频

- 主页和指令页共享单一视频管线；
- 两个视图能够同时获得最新帧；
- 页面切换不重启管线；
- 两个视图不竞争弹出同一RingBuffer帧；
- 小画面保持比例。

### 窗口

增加Qt Widgets GUI测试：

- 普通窗口展开/收起菜单后geometry不变；
- 普通窗口展开/收起告警栏后geometry不变；
- 最大化状态操作后仍最大化；
- 重复操作不产生窗口尺寸漂移。

## 十四、构建和验收

完成后执行：

1. Debug-x64构建。
2. Release-x64构建。
3. 全部现有测试和新增测试。
4. 主程序冒烟：
   - 首页视频；
   - 指令页小视频；
   - 页面切换；
   - Safe/姿态稳定联动；
   - Synchronization布局切换；
   - Stop/Move Pending及回退；
   - 舵机在所有推进器模式下仍可操作；
   - 菜单和告警栏窗口尺寸检查；
   - 正常退出且不破坏现有线程退出顺序。
5. 不得引入新的编译警告。
6. Mock测试通过只能声明“Windows侧和Mock A35通过”，不得声明真实板端对接完成。

## 十五、交付报告

报告必须包含：

- 修改文件；
- 新增/调整的funcId和状态字段；
- Safe/姿态稳定联动结果；
- 执行器ID映射；
- Stop/Move/Synchronization状态机说明；
- 视频复用实现；
- 窗口尺寸修复；
- Debug/Release构建结果；
- 测试总数及结果；
- 尚未完成的A35实机确认项；
- 是否修改了M33或A35——本任务预期必须为“否”。
  
## 十六、推进要求

开始本任务前，先设计工作Phase，并形成vibeplan文档。以Phase为断点，每phase完成后停止，待我编译通过后再继续执行。避免单个phase输出过多指令。如需引用外部文件，需将其复制至项目文件夹内再引用。代码内禁止硬编码，禁止引用绝对路径，Windows系统文件除外。