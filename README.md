# KratosRagePatch 0.1.0

为「斧头 God of War Kratos Combat MCO」增加独立怒气资源的补丁。**仅支持 Skyrim SE 1.6.640，SKSE 2.2.3。**

本包不包含原 MOD、动作文件或第三方运行时插件；必须保留原 MOD 及其前置。已完成的验证以包内 `Docs/KratosRage/verification.md` 为准。**未启动游戏进行实测，不能将离线测试通过理解为已通过游戏内验收。**

## 功能与默认规则

| 项目 | 默认值 |
| --- | --- |
| 怒气上限 | 100 |
| 普通命中 / 重击 / 特殊命中 | 2 / 4 / 3 |
| 受击 / 击杀 | 1 / 5 |
| 所有来源共享的滚动每秒上限 | 8 |
| 同目标、同来源的重复命中窗口 | 0.1 秒 |

- 仅玩家满足原装备条件时积攒；友军、己方召唤物、无效来源不计。特殊攻击采用明确白名单，不把切换武器、增益、粉尘或持续暴风雪算作直接攻击。
- 满怒才允许进入原变身动画。确认原变身效果生效后清空怒气，失败不扣；变身及待确认期间不积攒。
- 保留原操作方式，不新增热键、不新增 MCM。满怒条件追加在原 OAR 条件上，不替代 Keytrace 或装备条件。
- 独立怒气条；怒气不足时闪红，满怒时短暂红金色身体泛光。没有额外属性增益、全屏滤镜或提示音。
- 当前怒气写入 SKSE 共存档；正常读档不重新触发满怒反馈。

## 安装前置

必须使用与 **1.6.640** 兼容的版本：

1. 原「斧头 God of War Kratos Combat MCO」及其全部原有前置，确保不装本补丁时原动作与变身可用。
2. SKSE 2.2.3 和对应 AE Address Library。
3. SkyUI，提供 `SKI_WidgetBase` / `SKI_QuestBase`。
4. PapyrusUtil，提供 `JsonUtil`。
5. powerofthree's Papyrus Extender，提供 `PO3_Events_Alias` 命中和击杀事件。
6. Open Animation Replacer **2.3.4 或更新兼容版本**、Payload Interpreter，以及原套装使用的 `Keytrace.esp`。
7. 保留原套装已有装备前置，例如 `[full_inu] Mjolnir.esp`、`GuardianShieldSLN.esp`；`PPM PKiS.esp` 是原条件中的可选装备分支，并非本补丁新增硬性前置。
8. Microsoft Visual C++ 2015–2022 Redistributable（x64）。本 DLL 动态依赖 `MSVCP140.dll`、`VCRUNTIME140.dll` 和 `VCRUNTIME140_1.dll`，本包不附带这些运行库。

补丁 ESP 的显式 masters 为 `Skyrim.esm`、`Dragonborn.esm`；**这不代表可以移除运行时依赖的 `God of War.esp`、Keytrace 或上述插件**。

## MO2 安装

1. 先备份用于测试的游戏存档及其同名 `.skse` 文件。
2. 用 MO2 的「从压缩包安装」安装 `KratosRagePatch-0.1.0-skyrim-1.6.640.zip`，作为一个单独 MOD，不覆盖或合并原 MOD 目录。
3. MO2 左侧把本补丁放在原 MOD **下方**，使本补丁赢得文件冲突。需要赢冲突的文件为三个原 OAR 子组的 `config.json` 及 `SKSE/PayloadInterpreter/Config/God of War4.ini`。
4. 启用右侧的 `KratosRagePatch.esp`，建议放在 `God of War.esp` 后面；这是带 ESL 标记的 ESP，不占普通 ESP 槽位。
5. 从 MO2 启动原有 SKSE 启动项，自行完成下方验收。本补丁不新增行为文件或 HKX，不因安装本补丁单独要求重跑 Nemesis。

另外三个 `*Ready/config.json` 通过 OAR 复用原文件夹的动画。安装时不要移除、改名或重新分装原 `KratosMoveset` 文件夹，否则无法找到原动画。

若其他补丁也覆盖同一 OAR 配置或 `God of War4.ini`，仅调整顺序会使其中一份修改失效，需要人工合并；本包没有宣称兼容所有动作整合包。

## 配置

本补丁 MOD 内的路径：

```text
SKSE/Plugins/StorageUtilData/KratosRage/config.json
```

配置按 `rage`、`hud`、`feedback` 分组；数值说明见文件本身和上表。保持合法 JSON。HUD 默认位置 `x=80, y=650`，宽 `180`、高 `9`，使用 SkyUI 的 1280×720 逻辑坐标，不是显示器物理像素。自定义后重新加载存档应用设置。

- `rage.maximum` 与各项 `rage.gain`：怒气上限与收益。
- `rage.maxGainPerSecond`：所有来源共享的每秒收益上限。
- `hud`：位置、大小、颜色、透明度、脱战显示延迟和渐隐时间。
- `feedback`：拒绝闪红时长、反馈冷却、是否开启满怒泛光及其时长。

## 游戏内验收清单（尚未执行）

请使用测试存档逐项确认，而不是直接在唯一的长期存档上试验。

- 新游戏与已有存档都能初始化怒气条；切换符合/不符合条件的装备，显示与计分正确。
- 普攻、重击、白名单特殊攻击、受击、击杀按配置计分；密集多目标攻击不能绕过每秒上限。
- 打空气、格挡、打友军/己方召唤物、打尸体、持续区域伤害不会刷怒气。
- 怒气不足时尝试原变身操作，普通攻击可接手，怒气条短暂闪红，不扣原高额资源。
- 满怒只触发一次短暂泛光；原变身成功后怒气只清零一次，不再支付原变身的血量、精力和魔法费用。
- 变身前摇中断或施放失败不扣怒气；变身中不能积攒；结束后能重新积攒。
- 读档保留怒气、不重播满怒反馈；自定义上限大于 100 时，超过 100 的已存怒气不会丢失。
- HUD 的脱战渐隐、拒绝闪红、位置/大小及身体泛光外观符合预期。

游戏日志通常在 Windows「文档」下 `My Games/Skyrim Special Edition/SKSE/KratosRage.log`。若核心记录缺失，DLL 会记录原因并停止提供机制；不要据此反复覆盖原 MOD 文件。

## 暂停测试与回退

本补丁没有自动卸载清理程序。不要承诺从已经保存补丁脚本状态的存档中热卸载无残留。最稳妥的回退是关闭补丁并恢复安装前配套备份的 `.ess` 和 `.skse`。原 MOD 的磁盘文件未被本工程修改。

## 源码与离线构建

源码位于本项目，工具均使用项目内 `vendor` 和 `build` 目录。依赖 MSVC 2022 C++ 工具链、.NET 8、Python 3.11 或更新版本；构建顺序和实际验证记录见 `docs/verification.md`。生成 ESP 使用 Mutagen，不手写 ESP 二进制。

主要命令（从任意目录使用绝对路径运行）：

```text
python D:\Project\KratosRagePatch\tools\fetch_native_dependencies.py
python D:\Project\KratosRagePatch\tools\compile_papyrus.py
python D:\Project\KratosRagePatch\tools\build_ui.py
dotnet run --project D:\Project\KratosRagePatch\tools\PluginBuilder -c Release -- all --stage D:\Project\KratosRagePatch\build\stage --report D:\Project\KratosRagePatch\build\PluginBuilder\record-report.json --data "<游戏Data目录>" --gow "<原MOD内God of War.esp完整路径>"
python D:\Project\KratosRagePatch\tools\generate_oar.py --mod "<原MOD目录>" --stage D:\Project\KratosRagePatch\build\stage --report D:\Project\KratosRagePatch\build\PluginBuilder\oar-report.json
python D:\Project\KratosRagePatch\tests\test_package.py
python D:\Project\KratosRagePatch\tools\package_release.py
python D:\Project\KratosRagePatch\tools\verify_release.py
```

`fetch_native_dependencies.py` 只下载源码；fmt、spdlog 和 DLL 的 CMake 构建步骤见验证记录。发行脚本要求全部必要文件齐全、原 MOD 与接手哈希基线一致，拒绝覆盖已有同名发行包。包内 `Docs/KratosRage/manifest.json` 记录文件大小与 SHA-256。

## 第三方许可

DLL 静态链接/使用的 CommonLibSSE-NG、fmt、spdlog 和 rapidcsv 的许可证位于包内 `Docs/KratosRage/licenses/`。原 MOD、SkyUI、OAR、Payload Interpreter 等仍由各自原作者提供；本包不重新分发其资源或运行时二进制。
