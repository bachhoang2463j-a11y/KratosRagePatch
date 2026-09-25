# KratosRagePatch：模块、调试与扩展规格

规格版本：1.0 · 对应补丁：0.1.0 · 源码核对日期：2026-09-24。

**本文描述当前代码实际实现，不是新增功能承诺。** 目标仅为 Skyrim SE **1.6.640 / SKSE 2.2.3**。离线构建与验证已经完成；用户正在测试，本文没有将尚未反馈的游戏结果写成通过。

配套文档：

- [面向 Agent 的通用 Skyrim MOD 开发指南](skyrim-mod-development-agent-guide.md)：跨项目的选型、工程边界和验证准则。
- [README](../README.md)：安装前置、操作、配置入口及用户验收清单。
- [实现接口契约](implementation-contracts.md)：固定记录 ID、接口名与层间约定。
- [离线验证记录](verification.md)：完整构建命令、工具版本、实际测试证据。
- [续作进度](continuation-plan.md)：接手历史和当前发行状态。

## 1. 版本基线与维护边界

当前交付物：`dist/KratosRagePatch-0.1.0-skyrim-1.6.640.zip`，218,802 字节、23 个文件。

```text
SHA-256
968781652ea905010cb4ded7c8b8ecf4c090526c27bcf1718040599ce021cf06
```

正文中的“当前”“现有”均指此包对应的 0.1.0 实现。本文和通用指南是发行后新增的开发文档，**未写回该 ZIP，也不改变它的文件清单或哈希**。

维护约束：

- 原 MOD 只读；所有生成物在项目 `build`/`dist` 下。
- 用户测试期间不覆盖已交付 ZIP，不安装进 MO2，不改 profile，不操作游戏/桌面 UI。
- 游戏内实测、调试器附加、临时诊断包安装等需要另行明确授权；本文列出方法不等于已经执行。
- 不手写 ESP 生产二进制；更改 `PluginBuilder` 后重新生成。
- 不加热键/MCM，不重做原动作；不以删除原装备或 Keytrace 条件简化集成。
- 游戏运行时前置与 ESP masters 分开列出。`God of War.esp`、SkyUI、PapyrusUtil、PO3 Papyrus Extender、OAR、PI 等不能因不在 master 列表就移除。
- 本工程没有 Git 仓库；不要把本规格当作创建仓库或提交的授权。

## 2. 系统分层与模块地图

### 2.1 最重要的状态归属

**`RageState` 是怒气账本和事务状态的唯一权威。** `GameBridge` 持有它并把游戏事件转换为规则输入。Papyrus 读配置、维护生命周期并驱动表现；HUD 缓存只用于绘制，不能决定消费或变身成功。

```text
PO3 / alias 事件 ── KR_PlayerAlias ── KR_Native PSC / C++ 绑定 ──┐
SKSE 引擎事件 ────────────────────────────────────────────────┤
JSON ── KR_Config ── ConfigureRage ──────────────────────────┤
                                                            v
                                         GameBridge（任务队列、世界适配）
                                                            |
                                                            v
                                         RageState（唯一权威规则/事务）
                                                            |
                          ┌─────────────────────────────────┴─────────┐
                          v                                           v
            Ready Ability/MGEF → OAR 条件              SKSE ModCallbackEvent
                          |                                           |
                    原动画 / PI                          KR_Config / KR_PlayerAlias
                          |                                   |              |
                    请求 Spell 0x804                       KR_Widget     FullGlow EFSH
                          |                                   |
              Effect 0x805 → GameBridge                 meter.swf（只绘制）
```

### 2.2 原生模块

| 文件 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| [src/Plugin.cpp](../src/Plugin.cpp) | 版本导出、入口校验、日志、SKSE 消息及注册 | 计分规则、界面绘制 |
| [src/RageState.h](../src/RageState.h) / [cpp](../src/RageState.cpp) | 数值、去重、滚动限速、激活 token、保存恢复规则 | RE 对象、法术施放、UI |
| [src/GameBridge.h](../src/GameBridge.h) / [cpp](../src/GameBridge.cpp) | 表单解析、资格/来源判断、任务队列、激活确认、事件和 ready marker | 读取 JSON、绘制 HUD |
| [src/PapyrusBindings.cpp](../src/PapyrusBindings.cpp) | 注册 `KR_Native` 的 16 个 native 函数 | 单独维护资源副本 |
| [src/Serialization.cpp](../src/Serialization.cpp) | SKSE co-save 编解码及回调 | 保存 HUD 动画、配置文件、RE 指针 |
| [src/PCH.h](../src/PCH.h) | CommonLib/SKSE/spdlog 等预编译头 | 业务接口 |
| [CMakeLists.txt](../CMakeLists.txt) | rules 静态库、测试可执行文件、DLL 及 stage 复制 | MO2 安装 |

关键定位：`Plugin.cpp` 的 `SKSEPlugin_Load`；`GameBridge.cpp` 的 `ResolveForms`、`Queue`、`UpdateStateFromGame`、`RunActivationTransaction`；`RageState.cpp` 的 `TryAward`、`CommitActivation`、`Restore`。

### 2.3 Papyrus 与 UI

| 文件 | 职责 |
| --- | --- |
| [papyrus/KR_Native.psc](../papyrus/KR_Native.psc) | global native 声明；必须与 C++ 绑定一致 |
| [papyrus/KR_Config.psc](../papyrus/KR_Config.psc) | 唯一 JSON 读取者；范围校验/回退；下推规则与 HUD 设置；订阅资源事件；读档反馈宽限 |
| [papyrus/KR_PlayerAlias.psc](../papyrus/KR_PlayerAlias.psc) | PO3/控制/ModEvent 注册，装备和读档事件，pending/raging 单次更新链，身体泛光 |
| [papyrus/KR_Widget.psc](../papyrus/KR_Widget.psc) | SkyUI 生命周期，查询 native 快照，显隐/渐隐/临时显示策略，调用 SWF |
| [ui/src/KrMeterBootstrap.as](../ui/src/KrMeterBootstrap.as) | 暴露根 `widget` clip 和所有宿主/Papyrus 可调用方法 |
| [ui/src/KratosRageMeter.as](../ui/src/KratosRageMeter.as) | AS2 绘制、缩放/锚点、颜色、ready 脉冲、alpha 补间、拒绝闪红、HUD 模式可见性 |

身体泛光是 `EffectShader.Play`，不是 SWF 动画。SWF 的 ready 填充脉冲也不是游戏角色的 EFSH 泛光。

### 2.4 数据、生成器与验证程序

- [data/config.default.json](../data/config.default.json)：发行默认配置；运行时读的是虚拟 Data 下的副本。
- [data/integration.json](../data/integration.json)：人工核实的原记录、载荷和白名单审计事实。**DLL 不读取这个文件。**
- [tools/PluginBuilder/Analyzer.cs](../tools/PluginBuilder/Analyzer.cs)：读取原 ESP/ESM，生成记录分析及候选来源报告。
- [tools/PluginBuilder/Defaults.cs](../tools/PluginBuilder/Defaults.cs)：本机默认输入路径、插件名、预留记录 ID。
- [tools/PluginBuilder/PatchBuilder.cs](../tools/PluginBuilder/PatchBuilder.cs)：生成 ESP、VMAD、SEQ。
- [tools/PluginBuilder/Verifier.cs](../tools/PluginBuilder/Verifier.cs)：Mutagen 回读与结构契约检查。
- [tools/PluginBuilder/Program.cs](../tools/PluginBuilder/Program.cs)：`analyze`/`build`/`all` 命令入口。
- [tools/generate_oar.py](../tools/generate_oar.py)：只读原 MOD，生成六个 OAR JSON 与一个 PI INI。
- [tools/compile_papyrus.py](../tools/compile_papyrus.py)：合并 SDK 头、官方编译/汇编、第二前端、反汇编、接口与默认值检查、PEX staging。
- [tools/build_ui.py](../tools/build_ui.py)：MTASC 编译、SWF FWS/AVM1 结构及 PEX 调用合同检查。
- [tools/test_bridge.py](../tools/test_bridge.py)：复制真实桥接源码，以 fake RE/SKSE 接口编译测试；变异仅作用于副本。
- [tools/package_release.py](../tools/package_release.py)：默认配置 staging、严格运行文件清单、原 MOD 基线检查和不可覆盖 ZIP。
- [tools/verify_release.py](../tools/verify_release.py)：独立读取 ZIP/当前 stage/文档，检查 CRC、哈希、路径、默认配置、原 MOD 和五个布局探针。

## 3. 核心状态与不变量

### 3.1 规则层类型

`RageSettings` 的默认值：maximum=100，normalHit=2，powerHit=4，specialHit=3，receivedHit=1，kill=5，maxGainPerSecond=8。

`Snapshot` 字段：

| 字段 | 意义 |
| --- | --- |
| `current` / `maximum` | 当前怒气与当前规则上限 |
| `eligible` | 桥接最后一次刷新得到的资格 |
| `raging` | 已确认/恢复对账后的原变身状态 |
| `pending` | 存在活跃 token，或存档恢复出的待对账 pending |
| `ready` | `eligible && !raging && !pending && current >= maximum` |

`available_` 不属于 `Snapshot`，它在 `GameBridge` 中独立表示 `ResolveForms()` 的核心记录解析结果。

规则：

1. UI 不写账本；没有 `SetRage`/`AddRage` 对外接口。
2. 仅 eligible、非 raging、非 pending 时授予怒气。
3. 普攻/重击/特殊共用出击去重通道；受击、击杀各自单独通道。同通道、目标/对方 actor ID、来源 ID 在 **0.1 秒**内重复只计一次。
4. 所有来源共享 **1 秒滚动窗口**，超额按剩余预算裁剪，不是固定整秒桶。
5. 消费只发生在正确 token 的成功提交，或“恢复 pending 且真实效果已存在”的读档对账中；取消不扣。
6. 满怒通知由授予路径上的“未满 → 满”边沿产生，不是每次刷新都产生。配置把上限调低而使当前值达到上限，不会自行走一次 `becameFull` 授予反馈。
7. `RageState` 不持有锁；`GameBridge` 负责锁和执行边界。

### 3.2 固定时间参数

| 参数 | 当前值/单位 | 所在位置 |
| --- | --- | --- |
| 出击/受击/击杀通道内去重 | 0.1 秒 | `RageState::IsDuplicateHit` |
| 共享预算窗口 | 1.0 秒 | `RageState::PruneGainWindow` |
| 致命事件顺序容错窗口 | 0.25 秒 | `GameBridge::ProcessOutgoing` |
| 死亡后的近期敌对依据 | 2.0 秒 | `GameBridge::IsEligibleOpponent` |
| 命中/死亡上下文清理 | 5 / 30 秒 | `GameBridge::PruneContexts` |
| 激活请求节流 | 0.25 秒 | `RunActivationTransaction` |
| 激活确认 deadline | 0.5 秒 | `RunActivationTransaction` |
| pending/raging 观察更新间隔 | 0.5 秒 | `KR_PlayerAlias.RageEndWatchInterval` |
| 新游戏配置补推 | 1.0 秒后一次 | `KR_Config.StartupRepushDelay` |
| 读档后满怒表现宽限 | 2.0 秒 | `KR_Config.FullFeedbackLoadGrace` |

原生计时使用 `steady_clock` 秒数；Papyrus 使用 `Utility.GetCurrentRealTime()`；AS2 动画使用 `getTimer()` 毫秒。不要跨层比较原始绝对时间戳。

确认 deadline 不是“0.5 秒后必定触发的定时器”。原生效果事件可立即推进；否则由后续刷新检查期限。Papyrus 更新链在 VM 负载下可能晚于 0.5 秒执行。

## 4. 主要运行链路

### 4.1 加载与初始化

1. `SKSEPlugin_Version` 声明 0.1.0、仅 1.6.640、最低 SKSE 2.2.3、Address Library 和 post-629 结构。
2. `SKSEPlugin_Load` 再次检查 runtime，建立日志并注册 Papyrus、消息与序列化。
3. `kDataLoaded` 调用 `GameBridge::Initialize`：解析核心表单，注册 MagicEffectApply、Death、Combat 三类原生 event sink。
4. `KR_Config.OnInit` 读配置并推送；一秒后再补推一次，避免新游戏 native Revert 与脚本初始化的顺序使 `configured_` 留在 false。
5. 玩家 alias 初始化注册 PO3、唯一的攻击 control 和 ModEvent。注册方法只撤销本脚本相关订阅后重建。
6. `KR_Widget` 通过 SkyUI 注册。`OnWidgetReset` 重推 style、rage 和显示状态；普通配置/事件到来时若 SWF 尚未 ready，不能据此推断逻辑状态未更新。

**诊断关键：** `IsAvailable()` 为 true 只证明 native 核心记录解析条件满足，不证明 PO3、PapyrusUtil、SkyUI、OAR、PI 或所有可选装备表单都可用。若 DLL 根本未加载，调用 native 本身可能无法绑定，不会可靠地返回一个 false 供脚本处理。

### 4.2 命中、受击与击杀

- 出击：PO3 alias 事件 → `KR_Native.Submit*` → 捕获 FormID → `GameBridge::Queue` → 来源与目标检查 → `Award` → `RageState::TryAward`。
- 武器来源必须能解析为 `TESObjectWEAP`；flags 位 0/1 判格挡，位 16 判重击，位 5 判致命。Projectile 参数在现有武器入口中不用于评分。
- 魔法和投射物入口分别查自己的 `specialSources_` 通道白名单，不能因“有伤害”就计分，也不能在通道不匹配时得分。
- 受击只接收未格挡、敌对来源。source 为武器，或符合“FireAndForget、非 Self、至少一个 hostile+detrimental+Health 且 duration=0 的 effect”的 Spell。power/sneak/bash 三个布尔参数当前保留签名但不改变受击收益。
- 友军过滤排除玩家、teammate 及玩家/teammate 指挥的 actor；敌对状态可在死亡后利用短期上下文恢复。它不是“按派系名字判所有友军”的通用分类器。
- 击杀同时可能来自 PO3 与原生死亡事件，统一交给 `SubmitKill`；要求 victim 已死亡、killer 是玩家，并用死亡上下文的 `killAwarded` 避免重复。
- 授予成功后同步 ready marker、发 `KR_RageChanged`；刚达到上限才再发 `KR_RageBecameFull`。

目标资格的原生判断与具体 OAR 动画条件并非完全同一层。`IsAllowedEquipment` 当前允许：右手空/徒手且左手 Armor；右手有原斧头 keyword；右手处于 single 白名单；或双手都处于 paired 白名单。它不完整复刻每个 OAR 子组的按键/效果条件。

### 4.3 满怒、原动画和激活事务

1. `ready=true` 时 native 给玩家同步 Ability `KratosRagePatch.esp:0x802`，其效果为 `0x803`。这是 OAR 读取的标记，不是另一本资源账本。
2. Ready OAR 组保留其源组条件，只追加 `HasMagicEffect 0x803`，使用原动画文件夹。
3. 原动画的 `$SIBADA` 载荷先施放请求 Spell `0x804`，其 MGEF `0x805` 在玩家身上生效。
4. 原生 MagicEffectApply sink 只对“请求效果 + caster/target 均为玩家”的事件启动事务。
5. `TryBeginActivation()` 检查 ready 并返回唯一 token。桥接置 pending、撤销 ready marker、设 0.5 秒 deadline、发资源变化，再调用原变身 Spell `God of War.esp:0x814`。
6. 通过原 MGEF `God of War.esp:0x813` 的实际活跃状态确认成功，不能只看“调用施法 API 返回了”。
7. 成功时 `CommitActivation(token)` 清零一次、置 raging，再执行三个原成功载荷；未确认到期限/无法取得施法入口则取消，保留怒气。
8. pending/raging 时 alias 的单发更新链调用 `RefreshEquipmentState()`，推进迟到确认/超时并察觉原效果结束；两种状态都结束后更新链停止。

不足怒气时的闪红还有单独的**观察路径**：`OnControlDown("Right Attack/Block")` → `NotifyActivationAttempt`。当前 native 要求攻击状态，以及 forward、left、right 三个输入/对应 Keytrace 效果条件分别成立，再判断非 ready/非 pending/非 raging 才提示。`back` 参数当前未使用。不要将此接口描述为“可直接发起变身”或推断一个新热键。

`CastLikePayload` 调用 `CastSpellImmediate`；返回 true 只表示找到了 caster 并发起调用。成功 payload 的效果并未逐个确认或回滚；日志“resource consumed”也只对应怒气事务，不证明所有伴随视觉/攻击效果都成功。

### 4.4 保存、恢复与队列隔离

[Serialization.cpp](../src/Serialization.cpp) 的当前格式：

```text
serialization unique ID = 0x4B525247
record type             = 0x52414745
record version          = 1
payload                 = uint32 current, uint32 pending, uint32 raging
payload length          = 12 bytes
```

Load 先 Revert；忽略未知 type，拒绝错误版本/长度/短读，拒绝 current>10000 或非 0/1 状态位。无有效记录时保留重置后的状态。

未持久化：配置上限、资格、UI 动画、事件注册、去重窗口、任务队列、激活 token、RE 指针。

恢复顺序必须兼容 **Restore → Reconcile → Configure**：

- Restore/Revert 清桥接的活跃 token 和临时上下文、置 `configured_=false`、要求一次对账，并递增 `generation_`。
- 排队任务捕获旧 generation；执行时发现不匹配就丢弃，防止前一存档的攻击/变身在新世界落地。
- 恢复 pending 不是恢复旧 token。若真实 `0x813` 已存在，对账清零；否则保留怒气并解除挂起。
- `needsReconcile_` 保证重复 native/Papyrus 对账不会擦掉新产生的 pending。
- 存档值超过临时上限时，`loadClampSource_` 保留原始值，等权威配置到达再钳制。`Save()` 也保留此原值，避免“读档后立刻存档”固化损失。
- 读取存档不发一次新的满怒授予事件；表现层另有读档后 2 秒抑制。这个抑制也会挡住窗口内真实新到达的满怒反馈，是当前行为，不应描述成精准区分了反馈来源。

## 5. 对外接口与跨层契约

### 5.1 `KR_Native` 的 16 个函数

所有函数均为 Papyrus **Global Native**。精确参数以 [KR_Native.psc](../papyrus/KR_Native.psc) 与 [PapyrusBindings.cpp](../src/PapyrusBindings.cpp) 为准。

| 接口 | 作用 |
| --- | --- |
| `ConfigureRage(int maximum, int normalHit, int powerHit, int specialHit, int receivedHit, int kill, int maxGainPerSecond, float deniedInterval)` | 下推规则及拒绝冷却 |
| `SubmitWeaponHit(ObjectReference target, Form source, Projectile projectile, int flags)` | 武器出击 |
| `SubmitMagicHit(ObjectReference target, Form source, Projectile projectile)` | 魔法通道出击 |
| `SubmitProjectileHit(ObjectReference target, Form source, Projectile projectile)` | 投射物通道出击 |
| `SubmitIncomingHit(ObjectReference aggressor, Form source, Projectile projectile, bool powerAttack, bool sneakAttack, bool bashAttack, bool blocked)` | 受击 |
| `SubmitKill(Actor victim, Actor killer)` | 击杀 |
| `RefreshEquipmentState()` | 排队刷新资格、原效果与事务 |
| `NotifyActivationAttempt(string control, bool rightAttack, bool forward, bool back, bool left, bool right)` | 观察原操作并按条件拒绝提示 |
| `ReconcileAfterLoad()` | 排队进行一次读档对账 |
| `GetRage()` / `GetRageMaximum()` | 返回 int |
| `IsRaging()` / `IsReady()` / `IsEligible()` / `IsPending()` / `IsAvailable()` | 返回 bool |

变更入口大多先入队，getter 返回当前已应用的快照。**调用 `ConfigureRage()` 后紧接着 getter，不保证读到刚投递的设置。** 诊断需要看后续更新/回调，而不是把接口误当同步 RPC。

### 5.2 Native → Papyrus 事件

事件名：`KR_RageChanged`、`KR_RageDenied`、`KR_RageBecameFull`。

```text
回调签名：(string eventName, string strArg, float numArg, Form sender)
strArg：空字符串
numArg：发送当时的 current 怒气，非本次增量
sender：玩家
```

事件用于提醒读取最新状态。不能靠 numArg 叠加另一份计数，也不能假定三个事件互斥；一次授予可先 Changed 再 BecameFull，一次取消可先 Changed 再 Denied。

### 5.3 Papyrus → SWF

固定资源路径：`Interface/exported/widgets/KratosRage/meter.swf`。`GetWidgetSource()` 返回 `KratosRage/meter.swf`。SWF 为 AVM1/AS2、FWS v8，基准 1280×720 / 30 fps，根暴露 `widget` 子 clip。

业务方法：

```text
setPercent(number 0..1)
setReady(bool)
setStyle(fillColor, readyColor, width, height)
showMeter()
hideMeter()
flashDenied(durationSeconds)
cancelAnimations()
```

同时实现 SkyUI 需要的 `setHudMetrics`、`setRootPath`、`setClientInfo`、`setModes`、锚点/位置方法、`setAlpha`、`tweenTo`、`fadeTo`、`onModeChange`。不要只实现七个业务方法就认为可由 SkyUI 正常加载。

`KR_Widget.Ready` 是继承的**界面就绪**属性；`KR_Native.IsReady()`/SWF `_ready` 才是**怒气可发动**。二者同名近似但不是同一状态。HUD 尚未 ready 时 `FlashDenied()` 会直接返回，未缓冲该一次性反馈。

可见性策略：native 不可用则隐藏；临时显示窗口或 raging 可显示；否则要求 eligible 且玩家在战斗。退出显示条件后走脱战延迟与渐隐；满怒临时显示到期不再额外叠一次脱战延迟。宿主 HUD mode 的 holder 显隐是另一道门槛，可能在上述逻辑允许时仍挡住显示。

## 6. 配置协议

运行路径：`SKSE/Plugins/StorageUtilData/KratosRage/config.json`；JsonUtil 文件名为 `KratosRage/config`。必须确认 MO2 虚拟 Data 中哪个同名文件胜出。

文件为嵌套 JSON；读取用 `GetPathIntValue/GetPathFloatValue/GetPathBoolValue` 和 `.rage.maximum` 这类点路径，不能换成读取 JsonUtil 自有容器的普通 getter。

| JSON 字段 | 默认值 | Papyrus 接受范围 |
| --- | --- | --- |
| `rage.maximum` | 100 | int 1–10000 |
| `rage.gain.normalHit` | 2 | int 0–10000 |
| `rage.gain.powerHit` | 4 | int 0–10000 |
| `rage.gain.specialHit` | 3 | int 0–10000 |
| `rage.gain.receivedHit` | 1 | int 0–10000 |
| `rage.gain.kill` | 5 | int 0–10000 |
| `rage.maxGainPerSecond` | 8 | int 1–10000 |
| `hud.x` / `hud.y` | 80 / 650 | float −1280–1280 / −720–720 |
| `hud.width` / `hud.height` | 180 / 9 | float 1–1280 / 1–720 |
| `hud.fillColor` / `hud.readyColor` | 12728377 / 15646021 | int RGB 0–16777215 |
| `hud.alpha` | 90 | float 0–100 |
| `hud.outOfCombatDelay` | 3.0 | float 0–600 秒 |
| `hud.fadeDuration` | 0.4 | float 0–60 秒 |
| `feedback.deniedFlashDuration` | 0.35 | float 0–60 秒 |
| `feedback.deniedInterval` | 0.5 | float 0–60 秒；另见下述原生限制 |
| `feedback.fullGlowEnabled` | true | bool |
| `feedback.fullGlowDuration` | 1.0 | float 0–60 秒 |

实际行为：

- 文件缺失/不能解析用默认值；字段越界回退该字段默认值并 Trace，不是把它夹到边界。
- 原生对 maximum、收益、每秒预算再次钳制到上表相同区间。
- **拒绝冷却有不同的第二层范围：`GameBridge::Configure` 将其钳制到 0.1–5.0 秒。** 因此配置 0 不表示无冷却，配置 60 也不表示每分钟只能提示一次。
- HUD/泛光时长为 0 有关闭对应动画/泛光的语义，不能类推到 deniedInterval。
- 配置只读，不回写、不提供实时文件监视。现有 reload 时点是初始化、启动补推、每次玩家读档；编辑后重新加载测试存档应用。
- `KR_Config.AppliedMaximum`、`AppliedFullGlowDuration` 记录最近下推的值，供诊断观察；它们是 Auto 属性，不是收到 native 确认的回执。
- 编译脚本同时核对 20 个模板字段、`Default*` 属性和读取调用的路径/回退属性；改模板必须同步这些位置。

## 7. ESP、VMAD、OAR 和 PI 数据合同

### 7.1 新记录表

全部属于 `KratosRagePatch.esp`，带 ESL 标记、Form Version 44；下一预留 ID 为 `0x807`。

| local ID | 类型 / EditorID | 职责 |
| --- | --- | --- |
| `0x800` | QUST `KR_MainQuest` | 启动任务；KR_Config；强制玩家 alias 0 |
| `0x801` | QUST `KR_WidgetQuest` | 启动任务；KR_Widget |
| `0x802` | SPEL `KR_RageReadyAbility` | Constant/Self Ability，使用 0x803 |
| `0x803` | MGEF `KR_RageReadyEffect` | 隐藏、中性的 ready 标记；供 OAR 读取 |
| `0x804` | SPEL `KR_ActivationRequestSpell` | 免费、Self、FireAndForget；0x805 持续 1 秒，不加进玩家法术列表 |
| `0x805` | MGEF `KR_ActivationRequestEffect` | 中性请求标记；原生事件入口 |
| `0x806` | EFSH `KR_FullRageGlow` | 复制 Dragonborn.esm:0x21733 的记录，复用已有资源 |

`God of War.esp:0x805` 是斧头 keyword，与本补丁 `0x805` 不是同一个记录。任何调试信息至少保留 plugin + local ID。

当前显式 masters 集合为 Skyrim.esm / Dragonborn.esm；磁盘实际顺序由 Mutagen 写为 Dragonborn.esm / Skyrim.esm。文件内引用按实际 master 索引解析，不是用户运行时全局加载索引。不要仅因顺序与文档列举不同就手改二进制。

SEQ 当前为两个 little-endian uint32：`0x02000800`、`0x02000801`，对应文件内部“两条 masters 之后的自身索引”。它不是要求用户将补丁放在运行时第 02 槽，更不是手填 light runtime `FE...` 索引的模板。新增启动任务或改变 masters 时必须重新生成并验证 SEQ。

### 7.2 VMAD 固定绑定

```text
QUST 0x800 / KR_Config
  Widget   → QUST 0x801

QUST 0x800 / player alias 0 / KR_PlayerAlias
  Config   → QUST 0x800
  Widget   → QUST 0x801
  FullGlow → EFSH 0x806

QUST 0x801 / KR_Widget
  无自定义 VMAD 属性；继承 SkyUI 注册协议
```

objectFormat=2。表单对象属性使用 `[static int16][alias int16][formID uint32]`，alias=-1；玩家 alias entry 使用 alias=0，并携带所属任务自身的 FormID。生产由 Mutagen 写，Python 在测试里独立读取这些字节。

### 7.3 OAR 六文件合同

根路径：`meshes/actors/character/animations/OpenAnimationReplacer/KratosMoveset/`。

| 源子组 / 对应 Ready 组 | 覆盖限制 | 源优先级 | 保留条件数 |
| --- | --- | --- | --- |
| `SpartanRageTrigger` / `SpartanRageTriggerReady` | 原组禁用 mco_attack1–10 | 70300001 | 6 |
| `Barehanded` / `BarehandedReady` | 原组禁用 mco_weaponart | 60100100 | 2 |
| `LeviathanAxe` / `LeviathanAxeReady` | 原组禁用 mco_weaponart | 45124011 | 2 |

禁用项按 DefaultMale/DefaultFemale 两个行为项目写。Ready 组沿用源优先级、逐条保留各自源条件、只追加本补丁 0x803，并以 `overrideAnimationsFolder` 复用源文件夹。`SpartanRageTrigger2` 完全不覆盖。没有复制 HKX，也没有新增行为文件。

别把本表当通用 OAR 规则；它只描述已审计原 MOD 的三个组。原包结构变动时先重新分析，不把生成器报错当理由跳过核验。

### 7.4 PI 载荷分工

覆盖文件：`SKSE/PayloadInterpreter/Config/God of War4.ini`。

请求行：

```text
$SIBADA = @CAST|0x804|KratosRagePatch.esp|1|1|1|0|0|0|0|0|0
```

| 原载荷 | 当前处理者 |
| --- | --- |
| God of War.esp `0x817`、`0x844` | PI 原行逐字节保留；native 不重复施放 |
| God of War.esp `0x814` | native 激活事务发起，确认原 `0x813` 后提交 |
| Skyrim.esm `0x2FFD2`、God of War.esp `0x841`、Skyrim.esm `0x7A82B` | native 成功提交后的 `successCasts_` |
| 原 token `0X201DF97`，解析为 Dragonborn.esm `0x1DF97` | 实际为 MGEF，不能按该 Spell 载荷施放；移除而不升级为新效果 |

重写块是“请求行 + 两条保留行”。其他有效 `$SIBADA` 行移交/删除，注释及块外字节保持原状。`data/integration.json` 与独立测试记录此分工；不要只改 INI 就让两侧重复执行。

### 7.5 当前特殊攻击表

所有 ID 均为 `God of War.esp` 的 local ID。运行时权威在 `GameBridge::ResolveForms`，下列值仅作导航：

```text
Magic:
81F 841 845 84E 851 853 859 85A 85B 85C 85D 85F 862 867 86D

Projectile:
82B 840 842 86E
```

共 19 个白名单项。`integration.json` 将 31 个分析候选分成白名单与排除项；自动 channel hint 不是最终决策。切武器/增益、粉尘、持续区域 `0x843` 等明确不纳入。白名单缺失记录会被跳过，不会全部使 `IsAvailable=false`。

## 8. 如何 Debug：从加载到表现逐层定位

### 8.1 用户反馈最小包

每次问题记录至少包含：

```text
测试包文件名与 SHA-256：
Skyrim.exe 版本 / SKSE / 相关前置版本：
新游戏还是旧存档；是否从安装前备份开始：
装备、目标、攻击种类、操作步骤：
期望与实际；发生时间：
MO2 冲突胜出的 DLL / PEX / OAR JSON / PI INI / 配置来源：
KratosRage.log；必要时 Papyrus 日志/兼容的崩溃日志：
已改过哪些配置或排序（逐项列）：
```

用户正在测试时先收集现状，不要边收集边更换包。原包无异常的确认也有价值：它把“原动作链本来就坏了”与补丁回归分开。

### 8.2 真实可用的日志

native 日志由 SKSE 的 log directory API 定位，通常在 Windows“文档”下 `My Games/Skyrim Special Edition/SKSE/KratosRage.log`；Documents 重定向时按实际路径寻找。当前级别 info，info 及以上立即 flush。

| 日志片段 | 说明 |
| --- | --- |
| `KratosRage 0.1.0 loaded for Skyrim 1.6.640` | DLL 入口和注册流程已到末尾，不代表游戏玩法链全通 |
| `KratosRage records and event sinks initialized` | 核心记录解析成功并走到初始化末尾 |
| `Failed to register SKSE interfaces` | Papyrus/消息注册失败 |
| `Required original spell ... was not resolved` | 三个成功载荷之一解析不到 |
| `KratosRage disabled: required plugin records were not resolved` | 核心依赖解析失败，机制不提供 |
| `Rage effect was not confirmed; no rage consumed` | 到检查时未见原效果，事务取消 |
| `Rage activation confirmed and resource consumed` | 本地怒气事务成功提交 |
| `Failed to save rage state` | co-save 写入失败 |
| `Unsupported or truncated rage save record` / `Invalid rage save data` | co-save 格式/范围拒绝 |

当前**没有**逐次命中、白名单拒绝、装备资格、dedupe、token、generation 或所有 payload 成功结果的日志。不能据“没有 hit 日志”推断事件未到达。

Papyrus `Debug.Trace` 当前主要在配置读取/缺失/越界/空 Widget 与 alias unavailable 分支，前缀 `[KR_Config]` / `[KR_PlayerAlias]`。能否采集取决于实际 Papyrus 日志配置；日志未开启时不擅改 INI。详细报错需结合 native 是否绑定，不能只看前置脚本文件存在。

### 8.3 症状到模块的定位表

| 症状 | 先查什么 | 再定位到 |
| --- | --- | --- |
| 没有 native 日志 | runtime、SKSE loader 日志、DLL 胜出文件、VC x64 导入、入口早退 | Plugin.cpp |
| 有 loaded，但 disabled | 原 ESP/核心记录、masters 与动态依赖是否存在 | ResolveForms |
| HUD 与计分都不工作 | PEX/VMAD/alias 初始化、JSON 前置、configured 是否重推、事件是否注册 | KR_Config / KR_PlayerAlias |
| 装备后不涨怒 | native eligibility、source 实际类型/通道、是否敌对/格挡/尸体、pending/raging | GameBridge 事件适配 |
| 密集攻击收益少于简单相加 | 0.1 秒同通道去重与全部来源共享滚动预算 | RageState |
| 数值满了但原动画不触发 | ready marker、OAR 冲突胜出/源条件、Keytrace、原动画可用性 | SyncReadyMarker / OAR |
| 原动画出现但不消费 | PI 覆盖是否胜出、0x804/805 请求链、0x813 效果、超时日志 | PI / 激活事务 |
| 一直 pending 或结束后不涨 | alias 单发更新链、KR_RageChanged 订阅、RefreshEquipmentState 是否执行 | KR_PlayerAlias / GameBridge |
| 数值正确但 HUD 不见 | SkyUI Ready、WidgetRoot、SWF 胜出、HUD modes、eligible/战斗/alpha/坐标 | KR_Widget / AS2 |
| 闪红/泛光没有出现 | 一次性反馈发生时 UI ready、读档 2 秒宽限、feedback 设置、FullGlow VMAD | Config / alias / SWF |
| 改 JSON 不生效 | VFS 胜出文件、解析/点路径日志、读档重载、第二层范围 | KR_Config / Configure |
| 读档怒气异常 | `.ess`/`.skse` 是否成对、版本/长度拒绝、Restore→Reconcile→Configure | Serialization / RageState |

“资格显示 false”与“已满但 not ready”是两回事。ready 还受 pending/raging 控制；HUD 显示也不是计算资格的证据。

### 8.4 可选的临时诊断，不是现成命令

当前包**没有 MCM、控制台 debug 命令、trace 开关或作弊加怒入口**。通用控制台不能直接执行下面的 global native 调用。

如果普通日志不足，可在一个另行构建、标明版本的诊断 PEX 中加入以下采样，再由真实脚本事件调用：

```papyrus
Debug.Trace("[KR_DIAG] rage=" + KR_Native.GetRage() + "/" + KR_Native.GetRageMaximum() + ", available=" + KR_Native.IsAvailable() + ", eligible=" + KR_Native.IsEligible() + ", pending=" + KR_Native.IsPending() + ", raging=" + KR_Native.IsRaging() + ", ready=" + KR_Native.IsReady())
```

这段只是建议，**未加入当前四个 PEX**。它适用于 native 已绑定的场景；读到的是各次 getter 当时的状态，不是原子化的跨函数快照。不要在同一个调用栈里刚 Configure 就把旧结果当失败。

需要事件级诊断时，在桥接的接收/分类/拒绝/授予/事务边界加限流日志，记录来源 ID、时间、原因和前后状态；不是在 SWF 猜测规则。调试器仅在明确授权的测试进程中附加，并匹配同一构建的符号；当前发行 ZIP 不含 PDB。

## 9. 离线调试与构建命令

以下是 **PowerShell**，在工程内生成文件，不安装到游戏。完整依赖构建见 [verification.md](verification.md)。运行命令前确认没有其他构建者同时写同一 `build` 目录。

### 9.1 只测纯规则

```powershell
$root = 'D:\Project\KratosRagePatch'
$cmake = "$root\vendor\cmake\bin\cmake.exe"

& $cmake -S $root -B "$root\build\rules-only" -G 'Visual Studio 17 2022' -A x64 -DKR_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON
& $cmake --build "$root\build\rules-only" --config Release --target KratosRageTests
& "$root\vendor\cmake\bin\ctest.exe" --test-dir "$root\build\rules-only" -C Release --output-on-failure
& "$root\build\rules-only\Release\KratosRageTests.exe" "$root\tests\fixtures"
```

### 9.2 桥接测试和针对性变异

```powershell
python D:\Project\KratosRagePatch\tools\test_bridge.py
```

`--mutant retry`、`--mutant noepoch`、`--mutant reconcile` 是诊断测试本身的选项：分别移除等待窗口、generation 守卫或重复对账守卫，**预期失败**。只改 `build/bridge-tests/source` 副本。跑完变异后必须再运行不带 mutant 的命令，恢复普通测试副本/可执行文件；不要把 mutant 失败当生产回归。

`--no-build` 只跑现有 exe，不能用于证明当前源码已被测试。修改源后正常重建并核对日志中的源文件指纹。

### 9.3 脚本、HUD、插件与包检查

```powershell
$root = 'D:\Project\KratosRagePatch'

python "$root\tools\compile_papyrus.py"
python "$root\tools\build_ui.py"
python "$root\tests\test_package.py"
python "$root\tools\verify_release.py"
```

前两条会重新生成 PEX/SWF 并更新 staging；`--check` 只跳过 staging，仍会写编译/反汇编产物。**若只检查用户正在测试的固定发行物，不应为了“查看状态”重编译。** 重编后的 PEX 含新时间戳，当前工作树与老 ZIP 的逐字节审计可能不同，这是正常的版本漂移而非 ZIP 损坏。

改动 ESP/OAR/PI 时，运行 `PluginBuilder all` 与 `generate_oar.py`（带明确只读输入路径），再测试；只运行 `build` 不更新记录分析报告。原生改动要重建 DLL，不能只运行规则测试。

### 9.4 测试覆盖定位

| 测试/报告 | 覆盖 |
| --- | --- |
| [tests/RageStateTests.cpp](../tests/RageStateTests.cpp) + [hit_cases.json](../tests/fixtures/hit_cases.json) | 数值、窗口、资格、回退时钟 |
| [tests/ActivationTests.cpp](../tests/ActivationTests.cpp) + [activation_cases.json](../tests/fixtures/activation_cases.json) | token、成功/取消、重复提交、外部变身 |
| [tests/SerializationTests.cpp](../tests/SerializationTests.cpp) | 保存恢复、pending 对账、自定义上限保留 |
| [tests/bridge/BridgeTests.cpp](../tests/bridge/BridgeTests.cpp) | 真桥接源码的 mock 事件、同帧/延迟确认、旧任务隔离、重复 reconcile |
| [tests/test_package.py](../tests/test_package.py) | ESP/VMAD/SEQ 原字节，OAR/PI、白名单和 PSC/C++ 函数名集合，stage 布局 |
| `build/PluginBuilder/build-report.json` | Mutagen 49 项检查 |
| `build/papyrus/disasm/*.txt` / `build/ui/meter.tags.txt` | PEX 接口和 SWF 结构审计 |
| [tools/verify_release.py](../tools/verify_release.py) | ZIP 与当前工作树一致性、CRC/哈希、原 MOD 基线 |

当前通过基线：规则 37 个注册用例 + 27 个夹具、533 断言；桥接 19 用例/190 断言；包测试 53 项；插件 49 项。测试数量是当前基线，不是禁止新增测试的硬指标。

局限：包测试的 native 契约检查主要比较函数名集合，不完整证明参数类型或实际 VM 调用；SWF 字符串存在不证明方法实现正确；mock 不覆盖真实 RE/SKSE/VM 调度；官方与第二 Papyrus 编译也不替代游戏验收。`compile_papyrus.py` 的第二前端状态需要看日志，本脚本并非对所有交叉编译失败都返回失败码。

## 10. 如何扩展：最小修改面与必须同步项

任何扩展都先写新增行为、失败语义与兼容目标；下列是定位清单，不表示已经授权或实施这些功能。

### 10.1 只调整现有数值或 HUD 设置

用户实例配置内修改已存在字段即可，不必改源码。重新读档应用。发布默认值变更则同步：

1. `data/config.default.json`。
2. `KR_Config.psc` 对应 `Default*`；规则默认变动时同步 `RageState.h`，HUD 自身 fallback 也检查。
3. 数值夹具/默认规则测试、README 的默认值描述。
4. 官方 PEX、SWF（若涉及）、完整包测试后生成新版本包。

不要直接改 stage 默认 JSON；`package_release.py` 会拒绝它与模板不一致，而不是帮你覆盖。

### 10.2 新增配置字段

- 增加模板键、`Default*`、正确的 `Read*` 点路径、范围与应用位置。
- 更新 `compile_papyrus.py` 的 `CONFIG_MAP`；必要时更新函数/字符串断言。
- 如进入 native，更新 PSC/C++ 绑定、GameBridge、规则类型；如只影响 HUD，不必为了它修改 native 配置 ABI。
- 验证缺失、类型错误/越界、重载和默认值。存在不同层的范围时明确最终值。
- 公开 native 签名变更需要同步发行 DLL 和 PEX；不得只替换其中一个。

### 10.3 新增特殊攻击/装备来源

- 先运行原记录分析，确认 source 真正记录类型、施放/目标方式、是否持续区域/自增益、实际来自哪一类 PO3 事件。
- 更新 `GameBridge::ResolveForms` 对应白名单和 `data/integration.json` 的分区，保持候选/白名单/排除项一致。
- 更新 bridge fake world 的表单和 `source_whitelist`/友军/格挡/死亡/重复事件用例；同步包测试的跨文件一致性。
- 新装备涉及 native `IsAllowedEquipment` 与 OAR 各源条件的双层检查，不以单改 ready gate 替代装备条件。
- 新通道/新计分种类还要更新 `GainKind`、`GainFor`、去重通道和共享预算测试，明确是否与现有来源共预算。

### 10.4 新增 HUD 效果或身体反馈

HUD 修改点：`KratosRageMeter.as` 实现 → `KrMeterBootstrap.as` 暴露 → `KR_Widget.psc` 调用 → `build_ui.py.REQUIRED_STRINGS` 与 PEX 契约。验证未 ready、重建、重复触发、取消、读档和 HUD mode 隐藏时的行为。

身体反馈修改点：EFSH 生成记录、VMAD `FullGlow`、`KR_PlayerAlias` 播放逻辑、Config 字段。不要让 SWF 控制 actor 效果，也不要停止所有其他 MOD 的 shader。当前只对自己的 FullGlow 执行 Stop/Play。

UI 源码编译通过后，仍需授权的实际 Scaleform/游戏视觉验收；没有浏览器截图能替代这一步。

### 10.5 改激活条件、载荷或确认方式

同步审查：`RunActivationTransaction` / `ConfirmActivation` / `CompleteActivation`、规则 token API、PI keep/replace/drop 表、`integration.json`、OAR 原条件和 alias 更新链。

必须保留测试：

- 未满不能开始、pending 时重复请求不双扣。
- 同帧任务 drain 不马上回滚；延迟确认成功只扣一次。
- 施法入口缺失/效果未出现时不扣，超时可再次尝试。
- PI 保留的两条载荷不被 native 重复施放。
- 保存/恢复、外部变身、旧 generation 请求、重复 reconcile 不污染新事务。

若改超时时长，明确它与 VM 更新延迟、效果事件顺序的关系，不只把数值调大掩盖注册失败。

### 10.6 新记录、任务或存档字段

新记录同步 `Defaults.Id/NextFormId`、PatchBuilder、Verifier、`test_package.py.EXPECTED_RECORDS`；新启动任务还需同步 SEQ 的三处硬编码列表及验证。旧记录 ID 不重排。

存档变动同步 `SavedState`、`Save/Restore/Reconcile`、Serialization 的 version/长度/校验和迁移测试。当前 v1 不接受其他 version；新格式若要兼容老存档，必须明确读取 v1 的策略。不把 UI 状态和 RE 指针顺手放进 co-save。

### 10.7 支持另一游戏版本

这是兼容性项目，不是仅修改 `CompatibleVersions`。需要核验 SKSE、Address Library、CommonLib 结构/地址、原生调用、PO3/其他前置、记录/资源能力并为新 runtime 实测。1.6.640 的通过不能外推到 1.5.97、更新 AE、GOG 或 VR。

### 10.8 发行工具也有固定合同

新增运行文件/升级版本时同步：

- `package_release.py` 的 REQUIRED、版本/runtime、默认输出名、许可证/文档列表。
- `verify_release.py` 的版本/runtime、当前 16 个运行文件数量、允许的 DLL/文件范围。
- `test_package.py` 的 stage 顶层、OAR 文件集和相关断言。
- README、接口契约、spec、验证记录与发行哈希。

只把这两份开发文档加到 `docs` 不会自动打进 ZIP。以后若决定随包分发，需明确更新打包和审计两侧；当前已交付包保持不变。

## 11. 已知边界与禁止误判

1. 尚无用户游戏验收结果；无法宣称原费用、动画、Scaleform 外观或整合包兼容已实测。
2. `IsAvailable()` 不是全面依赖探针；missing DLL/native binding 失败也不是普通的 false 分支。
3. HUD 事件是表现提示，不是权威计数；UI Ready 与资源 ready 分开。
4. 请求排队不等于下一帧，deadline 不等于精准定时；单发更新链只在 pending/raging 存活，是有限状态观察，不是“完全不轮询”。
5. SEQ/FormID 高字节为文件内映射，不能据此要求用户改到特定加载槽；不要把推测写成已知加载顺序缺陷。
6. `data/integration.json` 改了不会自动改变 DLL；它是审计事实而非运行配置。
7. 完整包测试在原 MOD 缺失的机器上会跳过部分对原文件的检查；发行审计则要求基线和原 MOD 都存在。不能把带 skip 的运行说成同等覆盖。
8. 旧规则测试末尾仍有 `SPEC-GAP-EXPECTED-FAIL` 历史说明；以实际每项 PASS/FAIL 与退出码为准，不豁免失败。
9. 没有卸载清理、没有热卸载无残留保证。回退使用安装前成对备份 `.ess`/`.skse`。
10. 文档/注释与实际实现冲突时先复核源码和产物，不为了让文字成立去改用户正在测试的行为。

**接手完成条件：** 能指出事件在哪里进入、状态由谁拥有、失败在哪里保留资源、加载如何对账、界面从哪里读状态、发行物如何复核；提出的每个扩展都有对应的源码修改面与测试，而不是仅在某一层增加一个看似工作的入口。
