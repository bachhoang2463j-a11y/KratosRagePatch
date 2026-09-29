# KratosRagePatch 0.3.0

为「斧头 God of War Kratos Combat MCO」增加独立怒气资源的补丁。**仅支持 Skyrim SE 1.6.640，SKSE 2.2.3。**

本包不包含原 MOD、动作文件或第三方运行时插件；必须保留原 MOD 及其前置。已完成的验证见项目中的 [离线验证记录](docs/verification.md)，ZIP 内的 `Docs/KratosRage/manifest.json` 记录版本和文件哈希。**0.3.0 未启动游戏进行实测，不能将离线测试通过理解为已通过游戏内验收。**

发行 ZIP 共 21 个条目：16 个运行文件、4 个第三方许可证与 1 个 `Docs/KratosRage/manifest.json`；**不随包附带 README 或验证记录**。

## 功能与默认规则

| 项目 | 默认值 |
| --- | --- |
| 怒气上限 | 100 |
| 每个通过筛选的命中 | +2 |
| 默认上限所需有效命中 | 50 次 |

- 积攒输入为原版 `TESHitEvent`，不依赖 PO3；事件到达与实际处理两个时点都必须在活跃的战斗中，暂停、菜单、加载中、尚未完成读档对账、玩家死亡或非战斗时不授予。
- 只有玩家本人发起、目标为活着的 `Actor`、非自己、非队友/玩家或队友直接指挥的角色、对玩家敌对，且未标记为格挡时才计分。到达时目标已死亡则拒绝；到达时活着、入队后才死亡不因排队延迟拒绝，但仍需句柄有效并通过敌对/友军检查。
- 不做 0.1 秒同源去重，也没有每秒预算：同一目标的快速多段、多目标命中分别累计，总怒气仍封顶。免疫但产生未格挡命中事件的情况照常计分；无事件的原 MOD 特殊攻击不凭动画猜测命中。
- 不设装备积攒门槛，未完成配置时也按默认积攒；变身及待确认期间不阻止积攒，但仍阻止重复释放。
- 旧 `Submit*`、受击、击杀等入口均为无操作，不计分；击杀本身不额外奖励，也不做技能差异奖励。
- 满怒才允许进入原变身动画。确认原变身效果生效后清空怒气，失败不扣。
- 保留原操作方式，不新增热键、不新增 MCM。满怒条件追加在原 OAR 条件上，不替代 Keytrace 或装备条件。
- 独立怒气条；怒气不足时闪红，满怒时短暂红金色身体泛光。没有额外属性增益、全屏滤镜或提示音。
- 当前怒气写入 SKSE 共存档；正常读档不重新触发满怒反馈。17 个 Papyrus native 签名及存档格式 v1 与 0.2.0 保持一致。
- `TickCombat` 心跳只核对状态与激活事务，不再按时间加怒；脱战或玩家死亡清零。ESP、PEX、SWF、OAR/PI 和默认配置复用 0.2.0，因此 UI 组件日志仍显示 0.2.0。

## 安装前置

必须使用与 **1.6.640** 兼容的版本：

1. 原「斧头 God of War Kratos Combat MCO」及其全部原有前置，确保不装本补丁时原动作与变身可用。
2. SKSE 2.2.3 和对应 AE Address Library。
3. SkyUI，提供 `SKI_WidgetBase` / `SKI_QuestBase`。
4. PapyrusUtil，提供 `JsonUtil`。
5. Open Animation Replacer **2.3.4 或更新兼容版本**、Payload Interpreter，以及原套装使用的 `Keytrace.esp`。
6. 保留原套装已有装备前置，例如 `[full_inu] Mjolnir.esp`、`GuardianShieldSLN.esp`；`PPM PKiS.esp` 是原条件中的可选装备分支，并非本补丁新增硬性前置。
7. Microsoft Visual C++ 2015–2022 Redistributable（x64）。本 DLL 动态依赖 `MSVCP140.dll`、`VCRUNTIME140.dll` 和 `VCRUNTIME140_1.dll`，本包不附带这些运行库。

本补丁 0.3.0 使用原版命中事件，**不再需要 powerofthree's Papyrus Extender 提供 `PO3_Events_Alias`**。这只表示本补丁自身不再依赖 PO3，**不代表可以删除整合包或其他 MOD 需要的 PO3**，也不能据此移除原 MOD 的任何其它前置。

补丁 ESP 的显式 masters 为 `Skyrim.esm`、`Dragonborn.esm`；**这不代表可以移除运行时依赖的 `God of War.esp`、Keytrace 或上述插件**。

## MO2 安装

1. 先备份用于测试的游戏存档及其同名 `.skse` 文件。
2. 用 MO2 的「从压缩包安装」安装 `KratosRagePatch-0.3.0-skyrim-1.6.640.zip`，作为一个单独 MOD，不覆盖或合并原 MOD 目录。
3. MO2 左侧把本补丁放在原 MOD **下方**，使本补丁赢得文件冲突。需要赢冲突的文件为三个原 OAR 子组的 `config.json` 及 `SKSE/PayloadInterpreter/Config/God of War4.ini`。
4. 启用右侧的 `KratosRagePatch.esp`，建议放在 `God of War.esp` 后面；这是带 ESL 标记的 ESP，不占普通 ESP 槽位。
5. 从 MO2 启动原有 SKSE 启动项，自行完成下方验收。本补丁不新增行为文件或 HKX，不因安装本补丁单独要求重跑 Nemesis。

另外三个 `*Ready/config.json` 通过 OAR 复用原文件夹的动画。安装时不要移除、改名或重新分装原 `KratosMoveset` 文件夹，否则无法找到原动画。

从 0.2.0 升级时：**禁用旧的 0.2.0 补丁而不是两个同时启用**（运行资产相同，叠装无意义且易混淆），并在 MO2 冲突中确认 0.3.0 的 `SKSE/Plugins/KratosRage.dll` 胜出。

若其他补丁也覆盖同一 OAR 配置或 `God of War4.ini`，仅调整顺序会使其中一份修改失效，需要人工合并；本包没有宣称兼容所有动作整合包。

## 配置

本补丁 MOD 内的路径：

```text
SKSE/Plugins/StorageUtilData/KratosRage/config.json
```

配置按 `rage`、`hud`、`feedback` 分组；保持合法 JSON。HUD 默认位置 `x=105, y=710`，宽 `96`、高 `4`，使用 SkyUI 的 1280×720 逻辑坐标，不是显示器物理像素。自定义后重新加载存档应用设置。

- `rage.maximum`：怒气上限，同时决定默认满额所需的命中次数（默认 100 需 50 次）。
- `rage.gain.*`、`rage.maxGainPerSecond`：为兼容旧配置保留、仍被解析和钳制，但**不控制 0.3.0 的积攒**；每个通过筛选的命中固定 +2，没有 JSON 开关，也不做每秒限流。
- `hud`：位置、大小、颜色、透明度，以及脱战显示延迟和渐隐时间。`hud.outOfCombatDelay`、`hud.fadeDuration` 被解析保留，但当前 HUD 为常显策略，这两项不产生脱战渐隐效果。
- `feedback`：拒绝闪红时长、反馈冷却、是否开启满怒泛光及其时长。

## 游戏内验收清单（尚未执行）

请使用测试存档逐项确认，而不是直接在唯一的长期存档上试验。离线测试不能证明首击、致命击和特殊攻击覆盖这些引擎语义，以下必须由用户在游戏内完成。

- 新游戏与已有存档都能初始化怒气条并显示当前怒气。
- 一次普通命中是否 +2；同一目标快速多段、多目标命中是否分别累计；默认上限 100 是否需要 50 次有效命中。
- 首击是否计分；致命一击（目标在事件到达时仍活着、入队后才死亡）是否计分；到达时已死亡的目标是否拒绝。
- 原 MOD 特殊攻击是否产生未格挡命中事件并计分；没有事件时不按动画推断已命中。
- 打空气、格挡、打友军/己方召唤物、打自己、打尸体不会计分；免疫但仍产生未格挡命中事件时按规则计分。
- 暂停、菜单、加载中或读档尚未完成对账时的命中不计分。
- 没有命中时怒气不因等待或心跳增长；脱战或玩家死亡清零；读档后仅在仍处于战斗时保留已存怒气。
- 怒气不足时尝试原变身操作，普通攻击可接手，怒气条短暂闪红，不扣原高额资源。
- 满怒只触发一次短暂泛光；原变身成功后怒气只清零一次，不再支付原变身的血量、精力和魔法费用。
- 变身前摇中断或施放失败不扣怒气；变身及待确认期间仍能积攒；结束后能正常释放。
- 读档保留怒气、不重播满怒反馈；自定义上限大于 100 时，超过 100 的已存怒气不会丢失。
- HUD 为常显、十档、无数字（脱战不隐藏）；拒绝闪红、位置/大小及身体泛光外观符合预期。

游戏日志通常在 Windows「文档」下 `My Games/Skyrim Special Edition/SKSE/KratosRage.log`。若核心记录缺失，DLL 会记录原因并停止提供机制；不要据此反复覆盖原 MOD 文件。

## 暂停测试与回退

本补丁没有自动卸载清理程序。不要承诺从已经保存补丁脚本状态的存档中热卸载无残留。最稳妥的回退是关闭补丁并恢复安装前配套备份的 `.ess` 和 `.skse`。原 MOD 的磁盘文件未被本工程修改。

## 源码与离线构建

源码位于本项目，工具均使用项目内 `vendor` 和 `build` 目录。依赖 MSVC 2022 C++ 工具链、.NET 8、Python 3.11 或更新版本；构建顺序和实际验证记录见 `docs/verification.md`。生成 ESP 使用 Mutagen，不手写 ESP 二进制。

0.3.0 **只重建 DLL**，ESP/SEQ、PEX、SWF、OAR/PI 与默认配置直接复用 0.2.0、逐字节未改。本次原生构建与离线测试（从任意目录使用绝对路径运行）：

```text
cmake -S D:\Project\KratosRagePatch -B D:\Project\KratosRagePatch\build\native -G "Visual Studio 17 2022" -A x64 -DKR_BUILD_PLUGIN=ON -DBUILD_TESTING=ON
cmake --build D:\Project\KratosRagePatch\build\native --config Release --target KratosRage KratosRageTests
D:\Project\KratosRagePatch\build\native\Release\KratosRageTests.exe D:\Project\KratosRagePatch\tests\fixtures
python D:\Project\KratosRagePatch\tools\test_bridge.py
python D:\Project\KratosRagePatch\tests\test_combat_heartbeat.py
node D:\Project\KratosRagePatch\tests\test_ui_contract.js
python D:\Project\KratosRagePatch\tests\test_package.py
python D:\Project\KratosRagePatch\tools\package_release.py
python D:\Project\KratosRagePatch\tools\verify_release.py
```

`package_release.py` 要求全部必要运行文件齐全、原 MOD 与接手哈希基线一致，生成 ZIP 和同名 `.zip.sha256`，并拒绝覆盖任一已有文件；`verify_release.py` 重新检查 CRC、路径、文件集、SHA-256 伴随文件与原 MOD 基线，还会锁定 0.2.0 参考包哈希并逐字节核对所有复用资产。实际结果见 `docs/verification.md`。

**完整重建运行资源**（仅在需要再生 PEX/SWF/ESP/OAR/PI 时使用，本轮不执行）：`python tools\compile_papyrus.py`、`python tools\build_ui.py`、`dotnet run --project tools\PluginBuilder -c Release -- all --stage build\stage --report build\PluginBuilder\record-report.json --data "<游戏Data目录>" --gow "<原MOD内God of War.esp完整路径>"`、`python tools\generate_oar.py --mod "<原MOD目录>" --stage build\stage --report build\PluginBuilder\oar-report.json`；依赖下载与 fmt/spdlog CMake 步骤见 `docs/verification.md`。

## 第三方许可

DLL 静态链接/使用的 CommonLibSSE-NG、fmt、spdlog 和 rapidcsv 的许可证位于包内 `Docs/KratosRage/licenses/`。原 MOD、SkyUI、OAR、Payload Interpreter 等仍由各自原作者提供；本包不重新分发其资源或运行时二进制。
