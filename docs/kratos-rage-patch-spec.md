# KratosRagePatch：模块、调试与扩展规格

规格版本：1.2 · 对应补丁：0.2.0 · 源码基线：`0564148` · 核对日期：2026-09-28（合并接手时未提交的 1.1 草稿，补齐计时版说明与伤害链路复盘；1.1 没有独立 Git 快照）。

**本文描述该基线的实际实现，不是新增功能承诺。** 目标仅为 Skyrim SE **1.6.640 / SKSE 2.2.3**。0.2.0 已完成离线构建、回归和发行物审计；2026-09-28 用户反馈“经过测试换为时间链路 mod 已基本可以使用”。这是用户环境下的初步可用反馈，未附逐项验收步骤、时序或截图，**不等于全部边界和整合包兼容性均已验证，更不证明历史伤害链路可用**。已冻结包中的 `gameRuntimeTested=false` 记录的是打包时证据，不因后续反馈重写 ZIP。本次只更新文档，没有重新运行构建或游戏测试；证据清单见 9.4，伤害方案评估见第 12 节。

配套文档：

- [面向 Agent 的通用 Skyrim MOD 开发指南](skyrim-mod-development-agent-guide.md)：跨项目的选型、工程边界和验证准则。
- [README](../README.md)、[实现接口契约](implementation-contracts.md)、[离线验证记录](verification.md)、[续作进度](continuation-plan.md)：保留的早期说明和历史记录。它们尚未全部同步到 0.2.0；其中的命中计分、前置、HUD 和包版本描述不能覆盖本规格及当前源码。
- `build/complete-0.2.0/`：本机保存的 0.2.0 构建、测试、审计和预览证据；该目录被 Git 忽略，不保证在新克隆中存在。

## 1. 版本基线与维护边界

当前交付物：[KratosRagePatch-0.2.0-skyrim-1.6.640.zip](../dist/KratosRagePatch-0.2.0-skyrim-1.6.640.zip)，203,266 字节、21 个文件，其中运行文件 16 个、第三方许可证 4 个、manifest 1 个。

```text
SHA-256
72e4e8dd6a10d57abfc3aafcfaadf6c735f879bc7f001efb3d94c7e2e7eb23df
```

正文中的“当前”“现有”均指源码提交 `0564148` 对应的 0.2.0 实现。0.1.0 命中计分、0.1.1 伤害诊断和 UI-r1 分档修订属于历史版本，不与本版叠装。本文和通用指南**未写回已交付 ZIP，也不改变它的文件清单或哈希**。

本地仓库基线提交为 `944de33`，完整实现提交为 `0564148`，开发分支为 `fix/combat-timer-hud`。源码、测试、文档和 `dist` 发行物已纳入 Git；`vendor`、`build`、编译缓存被忽略。Git 提交不是远程备份证明，也不能替代这些外部输入的版本、哈希和重建记录。

维护约束：

- 原 MOD 只读；所有生成物在项目 `build`/`dist` 下。
- 用户测试期间不覆盖已交付 ZIP，不安装进 MO2，不改 profile，不操作游戏/桌面 UI。
- 游戏内实测、调试器附加、临时诊断包安装等需要另行明确授权；本文列出方法不等于已经执行。
- 不手写 ESP 生产二进制；更改 `PluginBuilder` 后重新生成。
- 不加热键/MCM，不重做原动作；不以删除原装备或 Keytrace 条件简化集成。
- 游戏运行时前置与 ESP masters 分开列出。`God of War.esp`、SkyUI、PapyrusUtil、OAR、PI 和原动作依赖不能因不在 master 列表就移除。**本补丁 0.2.0 不再依赖 PO3 命中事件或 Precision API**；这不代表可以删除整合包中其他 MOD 需要的这些前置。
- Git 已由用户授权建立。续作先核对工作树、当前提交和发行物哈希，完成改动后按项目约定提交；文档更新不得偷偷重编译或替换用户正在测试的 ZIP。

## 2. 系统分层与模块地图

### 2.1 最重要的状态归属

**`RageState` 是怒气账本和事务状态的唯一权威。** `GameBridge` 持有它并把游戏事件转换为规则输入。Papyrus 读配置、维护生命周期并驱动表现；HUD 缓存只用于绘制，不能决定消费或变身成功。

```text
KR_PlayerAlias 0.5 秒单发心跳 ── KR_Native.TickCombat ──┐
原攻击 control / 装备与读档事件 ── KR_Native 绑定 ──────┤
SKSE MagicEffectApply / Combat / MenuOpenClose ─────────┤
JSON ── KR_Config ── ConfigureRage ────────────────────┤
                                                       v
                              GameBridge（主线程队列、战斗/暂停状态、激活）
                                                       |
                                                       v
                              RageState（战斗计时、唯一账本、事务、存档）
                                                       |
                         ┌─────────────────────────────┴──────────┐
                         v                                        v
            Ready Ability/MGEF → OAR                  SKSE ModCallbackEvent
                         |                                        |
                   原动画 / PI                       KR_Config / KR_PlayerAlias
                         |                                  |             |
                   请求 Spell 0x804                     KR_Widget      FullGlow
                         |                                  |
              Effect 0x805 → GameBridge                meter.swf（只绘制）
```

旧 `Submit*`/PO3 路由和 `DamageHooks` 不在当前计分链上；保留名称是兼容措施，不表示有第二条积攒来源。

### 2.2 原生模块

| 文件 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| [src/Plugin.cpp](../src/Plugin.cpp) | 版本导出、入口校验、日志、SKSE 消息及注册 | 计分规则、界面绘制 |
| [src/RageState.h](../src/RageState.h) / [cpp](../src/RageState.cpp) | 战斗计时、数值、激活 token、保存恢复；保留旧规则函数供历史测试 | RE 对象、法术施放、UI |
| [src/GameBridge.h](../src/GameBridge.h) / [cpp](../src/GameBridge.cpp) | 表单解析、玩家战斗/暂停采样、释放资格、主线程队列、激活与 ready marker | 读取 JSON、绘制 HUD |
| [src/PapyrusBindings.cpp](../src/PapyrusBindings.cpp) | 注册 `KR_Native` 的 17 个 native 函数 | 单独维护资源副本 |
| [src/DamageHooks.cpp](../src/DamageHooks.cpp) / [h](../src/DamageHooks.h) | 保留的 0.1.1 伤害诊断实现；**不在当前 DLL 构建目标内，也不安装 hook** | 当前怒气增长 |
| [src/Serialization.cpp](../src/Serialization.cpp) | SKSE co-save 编解码及回调 | 保存 HUD 动画、配置文件、RE 指针 |
| [src/PCH.h](../src/PCH.h) | CommonLib/SKSE/spdlog 等预编译头 | 业务接口 |
| [CMakeLists.txt](../CMakeLists.txt) | rules 静态库、测试可执行文件、DLL 及 stage 复制 | MO2 安装 |

关键定位：`Plugin.cpp` 的 `SKSEPlugin_Load`；`GameBridge.cpp` 的 `TickCombat`、`IsUISuspended`、`IsLoadingTransition`、`Queue`、`UpdateStateFromGame`、`RunActivationTransaction`；`RageState.cpp` 的 `UpdateCombat`、`EndCombat`、`SuspendCombatTimer`、`CommitActivation`、`Restore`。

### 2.3 Papyrus 与 UI

| 文件 | 职责 |
| --- | --- |
| [papyrus/KR_Native.psc](../papyrus/KR_Native.psc) | global native 声明；必须与 C++ 绑定一致 |
| [papyrus/KR_Config.psc](../papyrus/KR_Config.psc) | 唯一 JSON 读取者；范围校验/回退；下推规则与 HUD 设置；订阅资源事件；读档反馈宽限 |
| [papyrus/KR_PlayerAlias.psc](../papyrus/KR_PlayerAlias.psc) | control/ModEvent 注册、装备和读档事件、持续的单发心跳、身体泛光；旧命中回调只调用无操作接口 |
| [papyrus/KR_Widget.psc](../papyrus/KR_Widget.psc) | SkyUI 生命周期，查询 native 快照，显隐/渐隐/临时显示策略，调用 SWF |
| [ui/src/KrMeterBootstrap.as](../ui/src/KrMeterBootstrap.as) | 暴露根 `widget` clip 和所有宿主/Papyrus 可调用方法 |
| [ui/src/KratosRageMeter.as](../ui/src/KratosRageMeter.as) | AS2 绘制、缩放/锚点、颜色、ready 脉冲、alpha 补间、拒绝闪红、HUD 模式可见性 |

身体泛光是 `EffectShader.Play`，不是 SWF 动画。SWF 的 ready 填充脉冲也不是游戏角色的 EFSH 泛光。

### 2.4 数据、生成器与验证程序

- [data/config.default.json](../data/config.default.json)：发行默认配置；运行时读的是虚拟 Data 下的副本。
- [data/integration.json](../data/integration.json)：人工核实的原记录、载荷与历史攻击白名单审计事实。**DLL 不读取这个文件，历史白名单也不再决定 0.2.0 的积攒。**
- [tools/PluginBuilder/Analyzer.cs](../tools/PluginBuilder/Analyzer.cs)：读取原 ESP/ESM，生成记录分析及候选来源报告。
- [tools/PluginBuilder/Defaults.cs](../tools/PluginBuilder/Defaults.cs)：本机默认输入路径、插件名、预留记录 ID。
- [tools/PluginBuilder/PatchBuilder.cs](../tools/PluginBuilder/PatchBuilder.cs)：生成 ESP、VMAD、SEQ。
- [tools/PluginBuilder/Verifier.cs](../tools/PluginBuilder/Verifier.cs)：Mutagen 回读与结构契约检查。
- [tools/PluginBuilder/Program.cs](../tools/PluginBuilder/Program.cs)：`analyze`/`build`/`all` 命令入口。
- [tools/generate_oar.py](../tools/generate_oar.py)：只读原 MOD，生成六个 OAR JSON 与一个 PI INI。
- [tools/compile_papyrus.py](../tools/compile_papyrus.py)：合并 SDK 头、官方编译/汇编、第二前端、反汇编、接口与默认值检查、PEX staging。
- [tools/build_ui.py](../tools/build_ui.py)：MTASC 编译、SWF FWS/AVM1 结构及 PEX 调用合同检查。
- [tools/test_bridge.py](../tools/test_bridge.py)：复制真实桥接源码，以 fake RE/SKSE 接口编译测试；变异仅作用于副本。
- [tests/CombatTimerTests.cpp](../tests/CombatTimerTests.cpp)：当前战斗计时、暂停、脱战清零与持久化边界。
- [tests/test_combat_heartbeat.py](../tests/test_combat_heartbeat.py)：在限定语句解释器中执行真实 Papyrus 心跳函数，验证单发链；不等于真实 VM 调度。
- [tests/test_ui_contract.js](../tests/test_ui_contract.js)：执行真实 AS2 方法的离线协议检查，覆盖十档显示、无数字、坐标、边框和重复刷新。
- [tools/package_release.py](../tools/package_release.py)：默认配置 staging、严格运行文件清单、原 MOD 基线检查和不可覆盖 ZIP。
- [tools/verify_release.py](../tools/verify_release.py)：只读核对 ZIP 与当前 stage、许可证及构建报告，检查 CRC、哈希、路径、默认配置、原 MOD 和五个布局探针。当前不把这两份 Markdown 文档列入归档或逐字节比较。

## 3. 核心状态与不变量

### 3.1 规则层类型

`RageSettings{}` 的 C++ 遗留初始化值仍是 maximum=100、normalHit=2、powerHit=4、specialHit=3、receivedHit=1、kill=5、maxGainPerSecond=8；发行 JSON/Papyrus 默认收益则是 2/2/2/0/0、预算 10。**二者都不是当前计时速率的定义**：0.2.0 仅 `maximum` 影响计时积攒上限，速度固定写在 `UpdateCombat` 为每累计 1 秒 +2，旧收益/预算字段只是兼容保留。

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

1. UI 不写账本；没有 `SetRage`/`AddRage` 对外接口。十档显示只改变填充长度。
2. 正常运行且不处于暂停/加载等屏蔽状态时，玩家 `IsInCombat() && !IsDead()` 才累计时间；每累计 1 秒 +2，封顶于 maximum。**不要求 eligible，也不因 raging/pending 本身停止积攒**；释放资格另行判断。
3. 首个有效心跳仅建立时间基准。后续使用相邻有效心跳的 `steady_clock` 差；非有限时间只停表，回拨或间隔大于 1.5 秒丢弃该间隔和未结算小数，不按长断档追补。
4. `SuspendCombatTimer()` 清时间基准、保留此前小数；恢复首个心跳不增长。`ClearTransient()` 清基准及小数，由 Reset、Restore、Reconcile、成功提交和 raging 状态切换调用。
5. 非战斗或死亡时 `EndCombat()` 清 current、pending、restoredPending、延迟上限源及瞬态计时，桥接同时作废激活 token。它**不主动 dispel 原变身效果**。脱战清零是资源重置，不是成功激活消费。
6. 激活消费只发生在正确 token 的成功提交，或“恢复 pending 且真实效果已存在”的读档对账中；普通取消不扣，但不能阻止随后脱战清零。
7. 满怒通知由计时授予路径的“未满 → 满”边沿产生。配置下调上限不会自行触发新的授予反馈；满额期间不囤积时间供消费后立即领取。
8. `RageState` 不持有锁；`GameBridge` 负责锁和执行边界。`TryAward`、`TryAwardPlayerDamage` 及旧去重/滚动预算仍有历史单测，但当前生产桥接没有调用它们。

### 3.2 固定时间参数

| 参数 | 当前值/单位 | 所在位置 |
| --- | --- | --- |
| 战斗采样单发链 | 请求间隔 0.5 秒，非精准调度 | `KR_PlayerAlias.RageEndWatchInterval` |
| 怒气结算 | 每累计 1.0 秒 +2 | `RageState::UpdateCombat` |
| 不可信心跳间隔 | 大于 1.5 秒或回拨时不补算 | `RageState::UpdateCombat` |
| 激活请求节流 | 0.25 秒 | `RunActivationTransaction` |
| 激活确认 deadline | 0.5 秒 | `RunActivationTransaction` |
| 新游戏配置补推 | 1.0 秒后一次 | `KR_Config.StartupRepushDelay` |
| 读档后满怒表现宽限 | 2.0 秒 | `KR_Config.FullFeedbackLoadGrace` |

原生计时使用 `steady_clock` 秒数；Papyrus 使用 `Utility.GetCurrentRealTime()`；AS2 动画使用 `getTimer()` 毫秒。不要跨层比较原始绝对时间戳。

确认 deadline 不是“0.5 秒后必定触发的定时器”。原生效果事件可立即推进；否则由后续刷新检查期限。Papyrus 更新链在 VM 负载下可能晚于 0.5 秒执行。

## 4. 主要运行链路

### 4.1 加载与初始化

1. `SKSEPlugin_Version` 声明 0.2.0、仅 1.6.640、最低 SKSE 2.2.3、Address Library 和 post-629 结构。
2. `SKSEPlugin_Load` 再次检查 runtime，建立日志并注册 Papyrus、消息与序列化。当前不编译或安装 `DamageHooks`。
3. `kDataLoaded` 调用 `GameBridge::Initialize`：解析核心表单，向脚本事件源注册 MagicEffectApply、Combat，向 UI 单独注册 MenuOpenClose。当前没有 Death sink；玩家死亡由状态采样识别。
4. `KR_Config.OnInit` 读配置并推送；一秒后再补推一次，避免新游戏 native Revert 与脚本初始化的顺序使 `configured_` 留在 false。配置决定释放资格等设置，不是计时增长的前置门槛。
5. 玩家 alias 的 `EnsureBridge()` 在 native 可用时注册唯一的攻击 control 和 ModEvent，无论是否可用都尝试维持一条 0.5 秒单发心跳。首个可用心跳补注册并重载配置；不注册 PO3。读档先清旧更新状态，再重建心跳、发起对账并重载配置。
6. `KR_Widget` 通过 SkyUI 注册。`OnWidgetReset` 重推 style、rage 和显示状态；普通配置/事件到来时若 SWF 尚未 ready，不能据此推断逻辑状态未更新。

**诊断关键：** `IsAvailable()` 为 true 只证明 native 核心记录解析条件满足，不证明 PapyrusUtil、SkyUI、OAR、PI 或所有可选装备表单都可用，也不检测 PO3。若 DLL 根本未加载，调用 native 本身可能无法绑定，不会可靠地返回一个 false 供脚本处理。

### 4.2 战斗计时与旧入口边界

1. `KR_PlayerAlias.OnUpdate` 调用 `KR_Native.TickCombat()`；该 native 只投递任务，实际在 `GameBridge::Queue` 的锁和 generation 守卫内处理。
2. native 采样玩家 `IsInCombat() && !IsDead()`，再由 `UpdateStateFromGame()` 更新装备资格、原变身效果和激活事务。玩家缺失、待对账或 Loading 过渡只停表，不把临时脱战状态写成资源清零。
3. 正常运行时将 `Now()`、战斗状态、`IsUISuspended()` 交给 `RageState::UpdateCombat`。每累计 1 秒 +2；不战斗/死亡则清零，暂停不补算。具体小数、长断档和满额规则见 3.1。
4. 授予或资源重置后同步 ready marker、发 `KR_RageChanged`；计时使怒气从未满达到上限时，再发 `KR_RageBecameFull`。SKSE Combat 事件用于补充刷新，不代替持续心跳。
5. 所有旧 `SubmitWeaponHit`、`SubmitMagicHit`、`SubmitProjectileHit`、`SubmitIncomingHit`、`SubmitKill` 和 C++ `SubmitPlayerDamage` 均为空实现。保留旧 alias 回调及函数签名，不代表仍依赖其事件，也不保证任意混搭旧 PEX/新 DLL 都兼容；实际运行应使用同一版本完整包。

**积攒资格与释放资格不同。** 当前计时不检查命中、敌我关系或装备。`IsAllowedEquipment` 只参与释放资格：右手空/徒手且左手 Armor；右手有原斧头 keyword；右手处于 single 白名单；或双手都处于 paired 白名单。它不完整复刻每个 OAR 子组的按键/效果条件。旧命中/受击/击杀规则见第 12 节，不作为 0.2.0 调试入口。

### 4.3 满怒、原动画和激活事务

1. `ready=true` 时 native 给玩家同步 Ability `KratosRagePatch.esp:0x802`，其效果为 `0x803`。这是 OAR 读取的标记，不是另一本资源账本。
2. Ready OAR 组保留其源组条件，只追加 `HasMagicEffect 0x803`，使用原动画文件夹。
3. 原动画的 `$SIBADA` 载荷先施放请求 Spell `0x804`，其 MGEF `0x805` 在玩家身上生效。
4. 原生 MagicEffectApply sink 只对“请求效果 + caster/target 均为玩家”的事件启动事务。
5. `TryBeginActivation()` 检查 ready 并返回唯一 token。桥接置 pending、撤销 ready marker、设 0.5 秒 deadline、发资源变化，再调用原变身 Spell `God of War.esp:0x814`。
6. 通过原 MGEF `God of War.esp:0x813` 的实际活跃状态确认成功，不能只看“调用施法 API 返回了”。
7. 成功时 `CommitActivation(token)` 清零一次、置 raging，再执行三个原成功载荷；未确认到期限/无法取得施法入口则取消，保留怒气。
8. alias 的常驻单发心跳通过 `TickCombat()` 内的状态刷新推进迟到确认/超时并察觉原效果结束；pending/raging 都结束后心跳仍继续，非战斗时也不撤销。不应将其描述为只在变身期间工作的观察链。

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

未持久化：配置上限、资格、UI 动画、事件注册、计时基准及未满一秒的小数、旧去重/预算窗口、任务队列、激活 token、RE 指针。

恢复顺序必须兼容 **Restore → Reconcile → Configure**：

- Restore/Revert 清桥接的活跃 token 和临时上下文、置 `configured_=false`、要求一次对账，并递增 `generation_`。
- 排队任务捕获旧 generation；执行时发现不匹配就丢弃，防止前一存档的攻击/变身在新世界落地。
- 恢复 pending 不是恢复旧 token。若真实 `0x813` 已存在，对账清零；否则保留怒气并解除挂起。
- `needsReconcile_` 保证重复 native/Papyrus 对账不会擦掉新产生的 pending。
- 存档值超过临时上限时，`loadClampSource_` 保留原始值，等权威配置到达再钳制。`Save()` 也保留此原值，避免“读档后立刻存档”固化损失。恢复后确认仍在战斗才能继续保留资源；真实脱战/死亡会正常清零，不把脱战重置误判为反序列化丢值。
- 读取存档不发一次新的满怒授予事件；表现层另有读档后 2 秒抑制。这个抑制也会挡住窗口内真实新到达的满怒反馈，是当前行为，不应描述成精准区分了反馈来源。

## 5. 对外接口与跨层契约

### 5.1 `KR_Native` 的 17 个函数

所有函数均为 Papyrus **Global Native**。精确参数以 [KR_Native.psc](../papyrus/KR_Native.psc) 与 [PapyrusBindings.cpp](../src/PapyrusBindings.cpp) 为准。表中合并的 getter 分别计数。

| 接口 | 作用 |
| --- | --- |
| `ConfigureRage(int maximum, int normalHit, int powerHit, int specialHit, int receivedHit, int kill, int maxGainPerSecond, float deniedInterval)` | 下推上限、拒绝冷却及兼容保留的旧收益字段；不改变固定计时速率 |
| `SubmitWeaponHit(ObjectReference target, Form source, Projectile projectile, int flags)` | 旧武器出击接口；无操作，不计分 |
| `SubmitMagicHit(ObjectReference target, Form source, Projectile projectile)` | 旧魔法接口；无操作，不计分 |
| `SubmitProjectileHit(ObjectReference target, Form source, Projectile projectile)` | 旧投射物接口；无操作，不计分 |
| `SubmitIncomingHit(ObjectReference aggressor, Form source, Projectile projectile, bool powerAttack, bool sneakAttack, bool bashAttack, bool blocked)` | 旧受击接口；无操作，不计分 |
| `SubmitKill(Actor victim, Actor killer)` | 旧击杀接口；无操作，不计分 |
| `TickCombat()` | 排队采样战斗/暂停状态，驱动唯一积攒通道并刷新事务 |
| `RefreshEquipmentState()` | 排队刷新资格、原效果、脱战/死亡状态与事务；不授予计时怒气 |
| `NotifyActivationAttempt(string control, bool rightAttack, bool forward, bool back, bool left, bool right)` | 观察原操作并按条件拒绝提示 |
| `ReconcileAfterLoad()` | 排队进行一次读档对账 |
| `GetRage()` / `GetRageMaximum()` | 返回 int |
| `IsRaging()` / `IsReady()` / `IsEligible()` / `IsPending()` / `IsAvailable()` | 返回 bool |

变更入口大多先入队，getter 返回当前已应用的快照；旧 `Submit*` 直接返回。**调用 `ConfigureRage()` 后紧接着 getter，不保证读到刚投递的设置。** 诊断需要看后续更新/回调，而不是把接口误当同步 RPC。C++ `SubmitPlayerDamage` 不是 Papyrus native，不计入上述 17 个接口。

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
setValue(current, maximum)  // 兼容旧调用的空方法，不显示数字
```

填充以 10% 为档：0 显示空条，正数按比例向上分档但未满最多显示 90%，实际比例达到 1 才画满。默认上限 100 时，2 点已经显示第一档，多个 +2 不一定每次改变条长；没有数字文本。是否使用 ready 色和脉冲仍取决于 native `IsReady()`，不由分档推断。

同时实现 SkyUI 需要的 `setHudMetrics`、`setRootPath`、`setClientInfo`、`setModes`、锚点/位置方法、`setAlpha`、`tweenTo`、`fadeTo`、`onModeChange`。不要只实现七个业务方法就认为可由 SkyUI 正常加载。

`KR_Widget.Ready` 是继承的**界面就绪**属性；`KR_Native.IsReady()`/SWF `_ready` 才是**怒气可发动**。二者同名近似但不是同一状态。HUD 尚未 ready 时 `FlashDenied()` 会直接返回，未缓冲该一次性反馈。

当前可见性以 [KR_Widget.psc](../papyrus/KR_Widget.psc) 的实际分支为准：`_diagnosticAlwaysOn=True`，读档也重置为 True，所以 native 可用时即使零怒、脱战或装备不合格也请求显示，**不能把脱战后空条仍在视为计时清零失败**。SkyUI Ready、宿主 HUD mode、alpha 和屏幕位置仍会影响最终是否可见。

脚本中保留了“临时显示/raging，否则 eligible 且战斗中”的旧显隐分支及脱战延迟，但当前常显分支优先返回，旧分支不是 0.2.0 默认策略；该布尔值也不是 JSON 配置项。

## 6. 配置协议

运行路径：`SKSE/Plugins/StorageUtilData/KratosRage/config.json`；JsonUtil 文件名为 `KratosRage/config`。必须确认 MO2 虚拟 Data 中哪个同名文件胜出。

文件为嵌套 JSON；读取用 `GetPathIntValue/GetPathFloatValue/GetPathBoolValue` 和 `.rage.maximum` 这类点路径，不能换成读取 JsonUtil 自有容器的普通 getter。

| JSON 字段 | 发行 JSON/Papyrus 默认值 | Papyrus 接受范围 |
| --- | --- | --- |
| `rage.maximum` | 100 | int 1–10000 |
| `rage.gain.normalHit` | 2（兼容保留） | int 0–10000 |
| `rage.gain.powerHit` | 2（兼容保留） | int 0–10000 |
| `rage.gain.specialHit` | 2（兼容保留） | int 0–10000 |
| `rage.gain.receivedHit` | 0（兼容保留） | int 0–10000 |
| `rage.gain.kill` | 0（兼容保留） | int 0–10000 |
| `rage.maxGainPerSecond` | 10（兼容保留） | int 1–10000 |
| `hud.x` / `hud.y` | 105 / 710 | float −1280–1280 / −720–720 |
| `hud.width` / `hud.height` | 96 / 4 | float 1–1280 / 1–720 |
| `hud.fillColor` / `hud.readyColor` | 12728377 / 15646021 | int RGB 0–16777215 |
| `hud.alpha` | 90 | float 0–100 |
| `hud.outOfCombatDelay` | 3.0 | float 0–600 秒 |
| `hud.fadeDuration` | 0.4 | float 0–60 秒 |
| `feedback.deniedFlashDuration` | 0.35 | float 0–60 秒 |
| `feedback.deniedInterval` | 0.5 | float 0–60 秒；另见下述原生限制 |
| `feedback.fullGlowEnabled` | true | bool |
| `feedback.fullGlowDuration` | 1.0 | float 0–60 秒 |

实际行为：

- `rage.gain.*`、`rage.maxGainPerSecond` 仍被解析、下推和钳制，但**不控制 0.2.0 的积攒**；每秒 +2 没有 JSON 开关。修改 `maximum` 会改变封顶值及理想充满时长，不改变速度。
- HUD 位置按 1280×720 逻辑输入映射到宿主 metrics，并在 SWF 中钳制到安全边界；96×4 是条体尺寸，不含外框，不是对所有分辨率的物理像素保证。十档显示不改账本，常显行为见 5.3。
- 文件缺失/不能解析用默认值；字段越界回退该字段默认值并 Trace，不是把它夹到边界。
- 原生对 maximum、收益、每秒预算再次钳制到上表相同区间。
- **拒绝冷却有不同的第二层范围：`GameBridge::Configure` 将其钳制到 0.1–5.0 秒。** 因此配置 0 不表示无冷却，配置 60 也不表示每分钟只能提示一次。
- HUD/泛光时长为 0 有关闭对应动画/泛光的语义，不能类推到 deniedInterval。
- 配置只读，不回写、不提供实时文件监视。现有 reload 时点是初始化、启动补推、每次玩家读档，以及 alias 首次发现 native 可用时的补推；不是每个心跳都重读。编辑后重新加载测试存档应用。
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

### 7.5 历史特殊攻击审计表（不用于 0.2.0 计分）

所有 ID 均为 `God of War.esp` 的 local ID。下表来自 [data/integration.json](../data/integration.json) 的早期来源审计，保留供回溯；当前 `GameBridge::ResolveForms` 不构建魔法/投射物计分白名单。

```text
Magic:
81F 841 845 84E 851 853 859 85A 85B 85C 85D 85F 862 867 86D

Projectile:
82B 840 842 86E
```

共 19 个历史白名单项。`integration.json` 将 31 个分析候选分成白名单与排除项；自动 channel hint 不是最终决策。切武器/增益、粉尘、持续区域 `0x843` 等当时未纳入。**表中有记录不证明真实命中回调到达，更不证明该记录本身造成了可归因的生命损失**；当前这些项不参与 `IsAvailable` 或怒气授予。

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
| `KratosRage 0.2.0 loaded for Skyrim 1.6.640; combat timer +2/s, clear on combat end; damage hooks disabled` | 当前 DLL 入口已到末尾，声明计时模式；不代表玩法链全通 |
| `KratosRage records and event sinks initialized` | 核心记录解析成功并走到初始化末尾 |
| `KratosRage combat heartbeat received` | 已进入有效玩家的心跳任务；不单独证明计时授予 |
| `KratosRage combat heartbeat: player entered combat` / `player left combat, rage cleared` | 采样战斗状态发生变化；日志早于加载/对账保护判断，清零须结合实际快照核实 |
| `KratosRage rage reached maximum via combat timer` | 计时授予使账本到达上限，并将发满怒事件 |
| `Failed to register SKSE interfaces` | Papyrus/消息注册失败 |
| `Required original spell ... was not resolved` | 三个成功载荷之一解析不到 |
| `KratosRage disabled: required plugin records were not resolved` | 核心依赖解析失败，机制不提供 |
| `Rage effect was not confirmed; no rage consumed` | 到检查时未见原效果，事务取消 |
| `Rage activation confirmed and resource consumed` | 本地怒气事务成功提交 |
| `Failed to save rage state` | co-save 写入失败 |
| `Unsupported or truncated rage save record` / `Invalid rage save data` | co-save 格式/范围拒绝 |

当前**没有**每个心跳的时间差/停表原因、每次 +2、装备资格、token、generation 或所有 payload 成功结果日志。旧命中路径已停用，不能通过查找旧 hit 日志判断当前计时是否工作；对历史版也须先确认该路径当时是否写日志，不能据“没有 hit 日志”推断事件未到达。

SWF 在收到宿主 metrics 时还可能经 `_global.skse.Log` 输出 `KratosRage meter 0.2.0 hud metrics: ...`，这是 UI 映射信息，不是原生怒气日志；缺失该行也不能单独证明没有绘制。

Papyrus `Debug.Trace` 当前主要在配置读取/缺失/越界/空 Widget 等分支，前缀为 `[KR_Config]`。alias 当前没有独立心跳 Trace，相关观察在 native 日志中。能否采集 Papyrus 日志取决于实际配置；日志未开启时不擅改 INI。详细报错需结合 native 是否绑定，不能只看前置脚本文件存在。

### 8.3 症状到模块的定位表

| 症状 | 先查什么 | 再定位到 |
| --- | --- | --- |
| 没有 native 日志 | runtime、SKSE loader 日志、DLL 胜出文件、VC x64 导入、入口早退 | Plugin.cpp |
| 有 loaded，但 disabled | 原 ESP/核心记录、masters 与动态依赖是否存在 | ResolveForms |
| HUD 与计时都不工作 | DLL/PEX 是否同版、VMAD/alias 初始化、单发心跳是否到达、核心记录是否可用 | KR_PlayerAlias / GameBridge |
| 在战斗但不涨怒 | `IsInCombat/IsDead`、Loading/对账、菜单/暂停、心跳间隔是否超过 1.5 秒；不先查旧 source 白名单 | TickCombat / UpdateCombat |
| 小幅涨怒但条长没变 | 十档显示、native 真实值是否增长；多个 +2 可能仍处于同一档 | RageState / AS2 |
| 脱战后数值清零但空条仍在 | 0.2.0 常显分支；资源清零与 HUD 隐藏是不同结果 | EndCombat / KR_Widget |
| 数值满了但原动画不触发 | ready marker、configured/装备资格、OAR 冲突胜出/源条件、Keytrace、原动画可用性 | SyncReadyMarker / OAR |
| 原动画出现但不消费 | PI 覆盖是否胜出、0x804/805 请求链、0x813 效果、超时日志 | PI / 激活事务 |
| 一直 pending 或结束后不涨 | alias 单发心跳、TickCombat 中状态刷新、加载/暂停和对账状态 | KR_PlayerAlias / GameBridge |
| 数值正确但 HUD 不见 | SkyUI Ready、WidgetRoot、SWF 胜出、HUD modes、alpha/坐标；当前不以 eligible/战斗状态隐藏 | KR_Widget / AS2 |
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
| [tests/CombatTimerTests.cpp](../tests/CombatTimerTests.cpp) | 当前计时授予、暂停/长断档、脱战清零、上限和持久化边界 |
| [tests/test_combat_heartbeat.py](../tests/test_combat_heartbeat.py) | 真实 Papyrus 函数体在受限解释器中的注册、重挂、读档重置和去抖 |
| [tests/RageStateTests.cpp](../tests/RageStateTests.cpp) + [hit_cases.json](../tests/fixtures/hit_cases.json) | 历史命中数值、去重、窗口、资格、回退时钟；不代表当前在用 |
| [tests/PlayerDamageTests.cpp](../tests/PlayerDamageTests.cpp) | 历史每次伤害事件 +2、滚动预算 10/秒；不验证引擎伤害输入 |
| [tests/damagehooks/DamageHookTests.cpp](../tests/damagehooks/DamageHookTests.cpp) | 历史 hook 在假 vtable/假引擎下的安装、过滤、血差与嵌套；独立测试，不在根 CMake 默认目标中 |
| [tests/ActivationTests.cpp](../tests/ActivationTests.cpp) + [activation_cases.json](../tests/fixtures/activation_cases.json) | token、成功/取消、重复提交、外部变身 |
| [tests/SerializationTests.cpp](../tests/SerializationTests.cpp) | 保存恢复、pending 对账、自定义上限保留 |
| [tests/bridge/BridgeTests.cpp](../tests/bridge/BridgeTests.cpp) | 真桥接源码的 mock 计时/菜单/死亡/加载事件、旧入口不计分、事务与队列隔离 |
| [tests/test_ui_contract.js](../tests/test_ui_contract.js) | AS2/Papyrus 协议、十档/无数字、映射及边框；不是游戏视觉验收 |
| [tests/test_package.py](../tests/test_package.py) | ESP/VMAD/SEQ 原字节，OAR/PI、历史审计事实、native 函数名集合、唯一计时入口与 stage 布局 |
| `build/PluginBuilder/build-report.json` | Mutagen 记录结构检查 |
| `build/papyrus/disasm/*.txt` / `build/ui/meter.tags.txt` | PEX 接口和 SWF 结构审计 |
| [tools/verify_release.py](../tools/verify_release.py) | ZIP 与当前工作树一致性、CRC/哈希、原 MOD 基线 |

已保存的 **2026-09-25 离线通过记录**如下，本次文档更新只读核对，未重新执行这些测试：

- `build/complete-0.2.0/rules-2.log`：65 个注册用例（含 16 个计时用例）+ 27 个夹具、881 断言，失败 0；旧命中/伤害用例仍包含在内。
- `build/complete-0.2.0/bridge-final.log`：21 用例、205 断言，失败 0；报告中的四个 `GameBridge`/`RageState` 源文件 SHA-256 与本次工作树相同。
- `build/complete-0.2.0/regressions.json`：UI 协议 209 检查、心跳 17 测试、包测试 53 项及 native CTest 均退出 0。
- `build/complete-0.2.0/release-audit.json`：插件检查 49 项、21 个归档条目、CRC/哈希和五个布局探针通过；`gameRuntimeTested=false` 是该次构建事实。
- `build/diagnostic-0.1.1/completion.json` 记载历史 damage hook 68 检查及 `gameRuntimeTested=false`。这个数量不能冒充当前默认回归，也不能证明 hook 在游戏中触发。

测试数量属于各自时间和版本的记录，不是禁止新增测试的硬指标。规则日志末尾的 `SPEC-GAP-EXPECTED-FAIL` 是历史说明，不代表此次存在预期失败；以上按逐项 PASS/FAIL 和汇总计数记录。`build`/`vendor` 被 Git 忽略，引用的本地报告在新克隆中可能不存在；不能把“本地看过”写成“仓库已归档全部证据”。

局限：包测试的 native 契约检查主要比较函数名集合，不完整证明参数类型或实际 VM 调用；SWF 字符串存在不证明方法实现正确；mock 不覆盖真实 RE/SKSE/VM 调度、引擎扣血时序或对象寿命；官方与第二 Papyrus 编译也不替代游戏验收。`compile_papyrus.py` 的第二前端状态需要看日志，本脚本并非对所有交叉编译失败都返回失败码。

### 9.5 用户实测状态与待留证边界

2026-09-28 用户已反馈改为时间链路后 MOD 基本可用。这一反馈作为正向游戏体验证据保留；以下是尚未附逐项结果的检查项，**不等于用户完全没有测试，也不表示本轮已执行**：

- 记录实际使用的包/DLL/PEX 哈希、runtime、SKSE 和 MO2 胜出文件，确保反馈对应 0.2.0。
- 连续活跃战斗中从首个有效心跳基准开始，默认上限理想约 50 秒充满；每累计 1 秒 +2，不受攻击频率影响。VM 延迟大于 1.5 秒会被丢弃，实际用时可能更长；十档条长不能替代精确数值/时序采样。
- 菜单、暂停、加载和长断档不增长、不补算；恢复首个心跳只建基准。
- 真正脱战或死亡后清零，原变身不被主动 dispel；HUD 空条仍可常显。不要把 Papyrus 请求间隔写成即时响应保证。
- 满怒正常触发一次反馈；激活成功只扣一次、失败不扣；pending/raging 本身不阻止计时，但仍阻止重复释放。
- 读档不在 Loading 过渡误清；恢复后仍在战斗才保留资源，真实脱战则清零。自定义上限、旧任务隔离、原费用/动画和 Scaleform 外观单独记录。

新的实测结果应追加日期、环境、步骤与日志，不回写冻结发行包中的旧验收标记。

## 10. 如何扩展：最小修改面与必须同步项

任何扩展都先写新增行为、失败语义与兼容目标；下列是定位清单，不表示已经授权或实施这些功能。

### 10.1 只调整现有数值或 HUD 设置

用户实例配置内修改**仍生效**的已有字段即可，不必改源码。重新读档应用。不要通过旧 `rage.gain.*` 或 `maxGainPerSecond` 调计时速度，它们已停用；若要改每秒 +2，应另行修改并测试 `UpdateCombat`，不是配置任务。发布默认值变更则同步：

1. `data/config.default.json`。
2. `KR_Config.psc` 对应 `Default*`；native 上限/规则默认变动时检查 `RageState.h`，HUD 同时核对 `KR_Widget` 与 AS2 fallback。旧收益默认差异见 3.1，不为文字一致而顺手改变历史规则。
3. 当前计时/桥接/默认配置测试及版本对应的说明；旧命中夹具仅在变更其历史规则时才改。
4. 官方 PEX、SWF（若涉及）、完整包测试后生成新版本包。

不要直接改 stage 默认 JSON；`package_release.py` 会拒绝它与模板不一致，而不是帮你覆盖。

### 10.2 新增配置字段

- 增加模板键、`Default*`、正确的 `Read*` 点路径、范围与应用位置。
- 更新 `compile_papyrus.py` 的 `CONFIG_MAP`；必要时更新函数/字符串断言。
- 如进入 native，更新 PSC/C++ 绑定、GameBridge、规则类型；如只影响 HUD，不必为了它修改 native 配置 ABI。
- 验证缺失、类型错误/越界、重载和默认值。存在不同层的范围时明确最终值。
- 公开 native 签名变更需要同步发行 DLL 和 PEX；不得只替换其中一个。

### 10.3 新装备资格与伤害方案不是同一扩展

- 新装备涉及 native `IsAllowedEquipment`、`ResolveForms` 中的装备集合与 OAR 各源条件的双层检查，不以单改 ready gate 替代原装备条件；同步装备资格与激活测试。
- 当前战斗积攒与攻击来源无关，新增攻击不需要向计时器登记。修改 `data/integration.json` 的历史攻击表不会使 DLL 增加计分来源。
- 若另行决定恢复命中/伤害积攒，先选定“命中定额、掉血事件定额、伤害量比例”口径和证据门槛，见第 12 节；不能只把 `DamageHooks.cpp` 加回 CMake，因为当前 bridge 入口为空、旧计分规则与脱战语义不同，测试也明确约束只有计时通道。
- 恢复时需明确敌我/召唤物、格挡/免疫、持续伤害、多目标、致命一击、去重和共享预算语义；本轮不实施这些改动，也不并行开启两个积攒来源。

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

1. 已有用户对 0.2.0 时间链路“基本可用”的实测反馈，但没有逐项留证；不能宣称原费用、全部动画、Scaleform 外观或整合包兼容均已验收。
2. `IsAvailable()` 不是全面依赖探针；missing DLL/native binding 失败也不是普通的 false 分支。
3. HUD 事件是表现提示，不是权威计数；UI Ready 与资源 ready 分开。当前 HUD 常显、十档且无数字，条长不变不必然代表账本不变。
4. 请求排队不等于下一帧，deadline 不等于精准定时；alias 单发心跳在非战斗、非 pending/raging 时也继续。它是低频采样，不是“完全不轮询”。
5. SEQ/FormID 高字节为文件内映射，不能据此要求用户改到特定加载槽；不要把推测写成已知加载顺序缺陷。
6. `data/integration.json` 改了不会自动改变 DLL；它是审计事实而非运行配置。
7. 完整包测试在原 MOD 缺失的机器上会跳过部分对原文件的检查；发行审计则要求基线和原 MOD 都存在。不能把带 skip 的运行说成同等覆盖。
8. 旧规则测试末尾仍有 `SPEC-GAP-EXPECTED-FAIL` 历史说明；以实际每项 PASS/FAIL 与退出码为准，不豁免失败。
9. 没有卸载清理、没有热卸载无残留保证。回退使用安装前成对备份 `.ess`/`.skse`。
10. 文档/注释与实际实现冲突时先复核源码和产物，不为了让文字成立去改用户正在测试的行为。

**接手完成条件：** 能指出事件在哪里进入、状态由谁拥有、失败在哪里保留资源、加载如何对账、界面从哪里读状态、发行物如何复核；提出的每个扩展都有对应的源码修改面与测试，而不是仅在某一层增加一个看似工作的入口。

## 12. 伤害积攒的修改历史、困难与完成难度

本节引用的 `build/` 日志、参考快照和 `vendor/` SDK 是本机只读核查材料，均被 Git 忽略，不保证新克隆可复原。仓库保留的是本节记录的摘录、定位及证据边界，不应宣称原始运行证据已完整归档。

### 12.1 历史实际证明了什么

复盘截至运行时代码基线 `0564148`：只有 `944de33` 和 `0564148` 两个 Git 提交，二者日期均为 2026-09-25。前者已是 0.1.1 诊断版快照，不是每次失败尝试的完整过程；0.1.0 的细节需结合旧发行包、早期说明和留存日志。后续文档提交不代表又发生了运行时迭代。

| 阶段 | 实际积攒口径 | 可追溯材料 |
| --- | --- | --- |
| 0.1.0（2026-09-24） | PO3 命中/受击/击杀事件，按类别固定给分，去重与共享预算 | 旧 README、`dist` 的 0.1.0 ZIP、历史规则/夹具、9 月 24 日游戏日志 |
| 0.1.1 diagnostic / UI-r1 | 原生生命伤害 hook；确认目标掉血后每次报告 +2，所有目标共享滚动 10/秒 | `944de33` 的 `DamageHooks.cpp`、`GameBridge.cpp`、诊断包和离线报告 |
| 0.2.0 | 活跃战斗时间每累计 1 秒 +2，脱战/死亡清零；伤害入口停用 | `0564148`、计时回归与发行审计、用户“基本可用”的反馈 |

**三种需求不能混称“伤害积攒”。** “命中就加固定值”只需要可靠命中事实；“确实造成掉血才加固定值”还要验证伤害与归因；“伤害越高加得越多”还要定义最终伤害量与换算。历史 0.1.1 属于第二种，而不是第三种：`GameBridge::AwardPlayerDamage` 当时只用 `healthLost>0` 验证，随后调用没有伤害数值参数的 `RageState::TryAwardPlayerDamage(now)`。当前保留的 [RageState.cpp](../src/RageState.cpp) 第 102–126 行仍能核对固定 +2 的规则。

时间版的成功没有证明旧版“差一个参数就能修好”，也没有证明伤害路线做不到。它证明的是换成更简单的输入后，现有账本、释放与表现链在用户环境中取得了初步可用结果。

### 12.2 已证实阻断与尚未确定的根因

**已证实：0.1.0 的一条运行前置在留存会话中未成功加载。** `build/diagnostic-0.1.1/evidence/skse64.log:88–89` 对 `po3_PapyrusExtender.dll` 记录 `no version data 0 (handle 0)`，全日志没有它的 `loaded correctly`，而本补丁正常加载。因此那次依赖 PO3 提供事件的路径没有可用的运行前置；不能因 SDK/PSC 文件能编译就认为事件已可用。为何该 DLL 没有可用版本数据未查明，不擅自归因于某个版本、缺失依赖或加载顺序，更不据此替用户更换整合包前置。

这份日志虽然放在 `diagnostic-0.1.1/evidence` 下，但配套 `KratosRage.log:1–2` 明确是 **2026-09-24 的 0.1.0**，只有 loaded 与初始化两行。它不是 0.1.1 hook 失效的证据。0.1.0 本身没有完整的逐次入口日志，“没有命中日志”也不能另行当作“收到了零次事件”的测量结果。

**尚未确定：0.1.1 hook 是否真实触发、在哪一步失去报告。** 已有 68 项离线检查，缺少与该版本匹配的游戏运行记录。不能从现有证据直接选定“槽位错了”“参数符号错了”“被其他 MOD 覆盖”“特殊攻击绕过了入口”中的任何一个作为根因。后续改用计时是已证实的工程选择，具体每轮放弃伤害方案的原因并没有完整归档。

### 12.3 真正困难的五个环节

#### 1. 入口能用，不等于覆盖了实际攻击

命中事件、伤害通知、生命值变化是不同观察点。已核对的 CommonLib `TESHitEvent` 含目标、攻击者、来源、投射物和 flags，**没有最终伤害量**；项目使用过的 PO3 alias 事件也属于事件/标志级接口。这可以支持限定范围的“命中定额”，不能直接宣称支持精确扣血计量。

原生方案避开了 PO3 运行前置，却转而依赖引擎函数、对象类型和插件链。当前 CommonLib `Actor.h:454` / `Actor.cpp:1723–1725` 对应 `HandleHealthDamage(Actor*, float)` 的 SE/AE 槽位 `0x104`，为签名和定位提供依据；它不证明本整合环境里的每类伤害都经过这里。这里只锁定 1.6.640，不把跨版本兼容算成已经解决，也不把跨版本需求强加给当前目标。

现有接口并非已被穷尽调查。若重启，应先核对目标环境可用的公开事件/框架 API，再决定是否继续自有 hook；不能从本仓库未找到接口推导出“整个生态都没有可复用方案”。

#### 2. 旧实现新增了一个没有游戏证据的扣血时序假设

[DamageHooks.cpp](../src/DamageHooks.cpp) 的 `ReportScope` 在原函数调用前读 `healthBefore`，原函数返回后读 `healthAfter`；`ReportHealthLoss` 只在 `before-after>0` 时上报。这要求真实扣血发生在这次原函数调用的内部。

如果该入口实际是扣血后的通知，或者生命变化发生在其他调用阶段，hook 即使正常触发也可能得到零差值，从而被第 82 行附近的判断静默过滤。**这是优先验证的高影响假设，不是已查明的 bug。** 不能在没有运行证据时直接删除血差判定，或直接把回调参数当最终扣血量。

参考材料也没有替我们验证这一点：本地 `build/diagnostic-0.1.1/refs/TrueHUD-Hooks.cpp:297–332` 用 `-a_damage` 显示伤害，没有采用本项目的前后血差门槛；该快照第 17 行的 `DamageHook::Hook()` 还是注释状态，且未保存精确上游 commit。它能支持“有这种观察写法、负参数按伤害处理”的参考依据，不能充当本 hook 已在游戏中工作的证明。

测试的边界尤其重要：[DamageHookTests.cpp](../tests/damagehooks/DamageHookTests.cpp) 第 121–150 行的模拟原函数会主动设置目标生命值。因此测试证明了“假定扣血就在这里发生时，报告和过滤正确”，没有独立证明引擎真的满足该前提。继续增加同一假设下的 mock 数量，不会填补这个缺口。

#### 3. 玩家归因和“一次伤害”的定义比加法复杂

旧 hook 第 62–71 行只接受有限负值、攻击者恰好为玩家、目标非玩家且调用前未死的输入。它不恢复 0.1.0 的敌我过滤，也没有在该入口提供武器/Spell/Projectile 来源；造成友军掉血同样可能符合条件。

原动作可能伴随多个效果和多目标，一次按键也不等于一次伤害报告。项目已有同目标嵌套折叠、不同目标分别报告与每秒预算，但这些规则不等于“同一招完整去重”；嵌套折叠还不能自动证明血差中每一部分都由玩家造成。若改为伤害量比例，是否计附魔、持续跳伤、召唤物、格挡后的少量伤害、击杀溢出，均需先定义再验证。

`integration.json` 的记录分析只能说明法术/效果候选，不能证明运行时归因。本次没有证据表明 God of War 的特殊攻击一定绕过该 hook，也没有证据证明全部都覆盖；`HookDamage.dll`、Precision 等出现在加载日志中，同样不是冲突或绕过的证据。

#### 4. 引擎适配的安全性必须在真实环境里验证

旧实现已做原函数链式调用、空入口保护、同线程重入栈和 ID/数值入队，这是可复用基础，不是从零开始。但 mock vtable 不能验证实际调用约定、对象在原调用后的寿命、其它 hook 的链式行为或真实调度。需要真实版本和同一构建的日志来定位，不能靠一行“hooks installed”验收。

按伤害量计分还要处理小数累计、实际减血与请求伤害的差异、目标剩余血量和预算裁剪。**乘一个比例本身很简单，难的是输入值是否代表约定的那种伤害。** 已有 `healthLost` 可作为实验材料，但不能未经验证就宣布比例版已完成大半。

#### 5. 当前最大的成本在观测与验证，不在再写一套 HUD

账本、上限、满怒通知、ready marker、激活 token、存档和原动画/HUD 集成都可继续复用，只需针对新语义回归。时间版只需定期读取玩家战斗状态和时钟，不必等待转瞬即逝的攻击事件，也不必重建攻击来源，因此显著缩短了故障定位范围；代价是空挥、输出高低不影响充能速度。

恢复伤害不是“把旧文件打开”：0.2.0 的 `SubmitPlayerDamage` 已为空实现，默认 DLL 不含 `DamageHooks.cpp`，桥接/包测试明确断言只由计时授予，还新增了脱战清零语义。合理路径是保留可用基线，先验证最小伤害入口，再决定替换；不是直接回退到旧诊断包并丢掉后续修复。

### 12.4 完成难度分档

以下是**工程判断，不是成功概率、完成百分比或工期承诺**。前提为沿用现有账本/集成，仅针对 1.6.640 与指定原 MOD；10 分表示需要大量引擎适配和运行验证。

| 目标 | 难度粗评 | 相对现状的主要新增工作 |
| --- | --- | --- |
| 保持时间链路并补足验收 | 2–3 / 10 | 已初步可用，补留证与边界确认 |
| 可靠命中后固定加值，限定攻击范围 | 4–5 / 10 | 验证一个可用事件源，明确过滤、覆盖与去重 |
| 确认玩家真实造成掉血后每次固定加值 | 6–7 / 10 | 验证 hook/替代接口、扣血时序和归因，再复用定额账本 |
| 按实际伤害量比例加值，并覆盖本项目特殊攻击 | 7–8 / 10 | 上一档基础上定义数值口径、部分伤害、小数与覆盖矩阵 |

若再要求任意整合包、任意脚本伤害、全部召唤物归因或多个 runtime，范围会明显扩大，不能沿用上表估算。反之，若最小探针证实只有一个简单时序判断或加载前置问题，限定方案的实现成本会下降；现有材料不足以给可靠天数或声称“已完成 80%”。

**建议：当前正式使用继续保留时间版。** 如果后续目标只是让“打中敌人”影响节奏，优先评估限定范围的命中定额，避免为此承担精确计量全部伤害的成本。若明确需要“实际掉血才有怒气”，再投入第二档入口验证；不要直接把范围扩大到所有来源的精确伤害比例。

### 12.5 将来重启伤害方案的最小验收门槛

以下只列研究门槛，本轮未生成诊断包、未安装或启动游戏：

1. **先证明能观测。** 在另行授权的测试中固定 DLL/PEX/前置版本，从一个玩家、一个普通目标、一次可重复普通攻击开始。使用独立版本标识，保留 0.2.0 包与测试存档。
2. **探针放在过滤之前。** 限流记录 hook/事件原始触发、attacker/target、原始参数、调用前后生命值及拒绝原因；再记录桥接是否入队、账本前后值和 HUD 快照。必须能区分未触发、非玩家来源、零血差、预算抑制与表现未更新。
3. **先对照引擎假设再恢复计分。** 若回调有伤害参数但血差为零，先核实扣血阶段和参数含义；若入口未触发，先查安装/类型/覆盖，不先改收益值。只有观察点语义成立才接回规则层。
4. **逐类扩大，不一次铺满。** 普攻通过后再验证徒手、原斧头/投掷、特殊法术、多目标、持续伤害和致命一击；对友军、召唤物、格挡/免疫等按约定记录正负案例与未覆盖项。
5. **只保留一个权威积攒通道。** 确定伤害方案后才替换计时授予、同步桥接与测试，明确脱战清零/变身期间积攒/预算语义，并回归激活、读档和 UI。是否替换属于新的功能决定，不由本分析自动授权。

复盘的核心结论是：**工程已有可复用的下游链路，但伤害输入还没有形成可追溯的游戏验证闭环。完成难度主要由这个缺口决定，而不是剩余代码行数。**
