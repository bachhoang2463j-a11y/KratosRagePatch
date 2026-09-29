# KratosRagePatch 0.3.0 离线构建与验证记录

验证日期：2026-09-29。目标仅为 **Skyrim SE 1.6.640 / SKSE 2.2.3**。本轮根据已确认的 [0.3.0 计划](../.zcode/plans/plan-sess_d06659d3-a57c-42ca-bfe3-5cbdbcd5327a.md) 续作；0.1.0 的原始记录保留在本文末尾，不能作为当前命中版的验收结论。

## 0.3.0 的实现与证据范围

- 接手时规则接口、版本号和部分测试已写入工作树，但 `GameBridge.cpp` 仍按战斗时间积攒。现已接通原生 `TESHitEvent`，每个通过筛选的命中固定 +2；`TickCombat` 只核对状态、处理脱战/死亡清零和激活事务，不再授予。
- 原生 sink 在事件到达时筛选并复制数值和目标句柄，任务执行时再次检查玩家、加载/暂停、句柄及敌我关系。目标到达时活着、入队后死亡不再按死亡状态拒绝；到达时已死则拒绝。菜单往返与已观察到的脱战会作废旧命中上下文；读档/Revert 使用现有 generation 守卫隔离全部旧任务。
- 同源快速多段、同帧多目标逐事件累计，没有每秒预算或 0.1 秒去重。日志中的 sequence 仅为观测编号，不是引擎唯一命中 ID。未配置、装备不合格、pending/raging 不额外阻止积攒。
- `TraceCombatHit` 按 `received`、拒绝原因、`awarded`、`at_maximum`、`became_full` 独立限流，每类每秒最多一条；字段包含 sequence、generation/context、cause/target/source/projectile ID、flags、实际增量、当前值/上限、分类累计次数及抑制次数。计数在进程内保留，抑制次数随该类下一条日志输出；日志限流不参与授予。
- 17 个 Papyrus native 的声明/绑定源码以及 `Serialization.cpp` 与 `0564148` 基线一致。存档仍为 v1、12 字节数值载荷，未新增配置、装备限制、技能奖励、伤害 hook 或 PO3 依赖。
- 本轮只编译 DLL 和 C++ 测试程序。ESP/SEQ、四个 PEX、SWF、OAR/PI 和默认配置不重编、不改变；15 个非 DLL 运行文件已与冻结 0.2.0 ZIP 逐字节比较。SWF 仍为 FWS v8，组件版本为 0.2.0。

**离线验证不等于游戏验收。** 未安装到 MO2，未修改 profile/INI/加载顺序，未启动 Skyrim、操作桌面或附加调试器。首击的战斗标志时序、致命击的死亡标志时序、特殊攻击实际是否发送事件、真实 SKSE/VM 调度及 Scaleform 外观仍待用户游戏测试。免疫但产生未格挡事件可计分是本版规则，不以实际掉血为门槛。

## 实际执行结果

以下均为本轮重新执行或针对本轮产物的检查，不沿用 0.2.0 的测试结论。

| 检查 | 结果 | `build/complete-0.3.0/` 中的证据 |
| --- | --- | --- |
| MSVC / CommonLibSSE-NG 原生编译 | Release DLL 与规则测试程序编译成功 | `native-configure.log`、`native-build.log`、`native-build-review.log` |
| CTest | 1/1 通过 | `ctest-final.log` |
| 规则、激活事务、存档及历史规则回归 | 75 个注册用例、27 个夹具；966 项断言，0 失败 | `rules-final.log` |
| 真实桥接源码的 mock 集成 | 47 个用例；334 项断言，0 失败 | `bridge-acceptance-restored-run.log` |
| 心跳单发链 | 17/17 通过 | `heartbeat-final.log` |
| AS2 / Papyrus UI 协议 | 209 项检查通过，未做视觉验收 | `ui-contract-final.log` |
| ESP/VMAD/SEQ、OAR/PI、接口及包契约 | 53/53 通过，无跳过项 | `package-contract-final.log` |
| 三种桥接故障变异 | 均被测试检出，正常副本已恢复 | `bridge-acceptance-mutations.json` |
| Python 语法 | 已变更的测试/发布工具编译检查通过 | `syntax-final.log` |
| Native ABI 与存档兼容 | 17 个声明/绑定与序列化源码匹配 `0564148` | `compatibility.json` |
| DLL 二进制静态检查 | x64 PE32+；SKSE 两导出；实际插件 0.3.0.0；仅 1.6.640；最低 SKSE 2.2.3；stage 与 Release 一致 | `native-pe-initial.log`、`native-metadata.json` |
| 旧发行包保护 | 四个旧 ZIP 的整体哈希与各自 SHA 文件一致；未修改 | `frozen-releases-before.json` |
| 原 MOD 基线 | 1,013 个路径与 SHA-256 均与原基线一致 | `original-preflight.json` |

变异结果必须与正常回归分开解读：

- `retry` 故意去掉确认等待窗口：7 个用例、25 项断言失败，包含 `pending_survives_same_frame_drain`。
- `noepoch` 故意去掉 generation 守卫：3 个用例、7 项断言失败，覆盖旧 Configure 泄漏及在读档对账**已经执行完成后**再运行旧命中闭包的 Restore/Revert 两条路径。
- `reconcile` 故意去掉重复对账守卫：1 个用例、3 项断言失败。
- 以上变异只作用于 `build/bridge-tests/source`。最终 `none` 版本 47/334 全过，副本与生产源码逐字节一致，生产源码指纹在整轮变异期间未变。
- 首轮变异的外层检查曾返回 1：新旧命中测试正确抓住 `noepoch`，但旧请求用例因另一层 `needsReconcile_` 保护仍通过，未能检测旧配置导致的资格泄漏。补上 `eligible/ready/marker` 三项断言后重新执行全套，全部达到预期；没有放宽产品断言或把普通失败豁免。首轮日志保留在 `bridge-mutations.json`。

当前 DLL 依赖 `MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` 及 Windows 系统运行库，与 README 的 x64 VC 运行库前置一致。静态读取 DLL 没有加载它或执行 SKSE 入口，不能据此声称游戏加载成功。工具链为 VS2022 Community / MSVC 14.44.35207、Windows SDK 10.0.26100.0、CMake 3.31.8、Python 3.14.3、Node.js 24.12.0；详见 `toolchain.json`。

## 只读审查与修正

两名独立只读代理分别审查命中规则/桥接/测试与发布工具/文档，均无阻断性实现问题。已落实的修正包括：跨读档旧 Configure 的资格/marker 断言、对账后旧命中闭包的 generation 判别、满怒读档不重播边沿、自动生成并核验 SHA-256 伴随文件、冻结参考包内容校验，以及安装/常显 HUD/组件版本等说明同步。

规则接口按计划保留 `TryAwardCombatHit(inCombat, suspended)`。桥接已先验证两个条件，故生产调用传入 `true, false`；规则层的参数边界测试证明规则接口行为，真正的暂停/脱战路径由桥接集成用例验证，二者不混称为同一层覆盖。历史命中/伤害/计时接口按计划保留为测试对象，不接回生产。仍未单独模拟“句柄解析成另一个 FormID”及“必需记录缺失导致 available=false 后到达命中”的两个拒绝分支；这不改变需要游戏内确认真实对象寿命和事件覆盖的边界。

## 正式交付与发布审计

- 发布包：[KratosRagePatch-0.3.0-skyrim-1.6.640.zip](../dist/KratosRagePatch-0.3.0-skyrim-1.6.640.zip)
- 校验文件：[同名 .zip.sha256](../dist/KratosRagePatch-0.3.0-skyrim-1.6.640.zip.sha256)
- ZIP 大小：**207,309 字节**；共 **21 个条目**，其中 16 个运行文件、4 个第三方许可证、1 个 manifest。README 与验证说明在项目中单独交付，不在 ZIP 内。
- 整体 SHA-256：

```text
5f89a587e5eb62ba1989877f6fd68173e108c8698972034b4ba553ad402d30a2
```

Release DLL 为 405,504 字节，SHA-256 为 `0090375c2b43e49a66ff60d745e88c323ec4cd5a2a95fe510fa52e8a0ed1e8b0`。正式 ZIP 与先行审计的候选 ZIP 逐字节一致；`release-audit.json` 确认 CRC、路径安全/大小写唯一性、manifest 的 20 项大小及哈希、SHA 伴随文件、stage/Release DLL 一致。manifest 明确标注 `gameRuntimeTested: false`。

冻结 0.2.0 ZIP 的 SHA-256 仍为 `72e4e8dd6a10d57abfc3aafcfaadf6c735f879bc7f001efb3d94c7e2e7eb23df`。15 个非 DLL 运行文件加 4 份许可证共 **19 个复用条目**全部逐字节一致；只有 DLL 和 manifest 内容改变。原 ESP 未重新生成，现有 Mutagen 49 项记录检查报告与本轮独立 ESP/VMAD/SEQ 原字节测试共同核验该复用产物，不把历史报告冒充本轮重新运行生成器。

发行工具的 7 个正反例已执行并通过（`release-tool-probes.json`）：合法候选包及自动 SHA 接受；同名 ZIP 拒覆盖且 ZIP/SHA 不变；只有同名 SHA 已存在时也拒绝打包；错误 SHA、缺失 SHA、内容被改变的参考包均拒绝；相同冻结包的另一文件路径可接受。5 个 JSON 布局探针也全部符合预期，三个负向探针的预期断言失败不是产品回归。

最终核对四个历史 ZIP 全部未变，原 MOD 的 1,013 个文件仍与原基线一致。发布后再次执行包契约 53/53 通过，更新后的 Python 工具语法检查通过。对应日志为 `release-package.log`、`release-audit.json`、`package-contract-after-release.log`、`syntax-after-review.log` 与 `postrelease-checks.json`。

源码、测试、文档、计划和正式 ZIP/SHA 随本次本地 Git 提交备份；不执行 push。外部 `vendor`/`build` 输入与本机完整日志仍被 Git 忽略，交付回复记录实际提交短 hash。

## 本轮可复跑命令

以下为 PowerShell，复用工程内已有工具链和依赖，不下载安装系统组件，不重新生成冻结运行资产。

```powershell
$root = 'D:\Project\KratosRagePatch'
$cmake = "$root\vendor\cmake\bin\cmake.exe"
& $cmake -S $root -B "$root\build\native" -G 'Visual Studio 17 2022' -A x64 -DKR_BUILD_PLUGIN=ON -DBUILD_TESTING=ON
& $cmake --build "$root\build\native" --config Release --target KratosRage KratosRageTests
& "$root\vendor\cmake\bin\ctest.exe" --test-dir "$root\build\native" -C Release --output-on-failure
& "$root\build\native\Release\KratosRageTests.exe" "$root\tests\fixtures"
python "$root\tools\test_bridge.py"
python "$root\tools\test_bridge.py" --mutant retry
python "$root\tools\test_bridge.py" --mutant noepoch
python "$root\tools\test_bridge.py" --mutant reconcile
python "$root\tools\test_bridge.py"
python "$root\tests\test_combat_heartbeat.py"
node "$root\tests\test_ui_contract.js"
python "$root\tests\test_package.py"
python "$root\tools\package_release.py"
python "$root\tools\verify_release.py"
```

三个 mutant 命令应返回失败，以证明回归能检测故意注入的错误；它们只改隔离副本，之后必须不带 mutant 重跑以恢复普通源码与可执行文件。已存在正式 ZIP 时，打包命令应拒绝覆盖；复现构建可用 `--output` 指定另一个新文件。`verify_release.py` 当前针对 0.3.0，不再用于审计历史 0.1.0 包。

本轮日志保存在 `build/complete-0.3.0/`。`build` 和 `vendor` 被 Git 忽略，原始本机日志不等于已随源码仓库备份。C++ 规则测试同时包含历史命中、伤害和计时规则；日志末尾历史 `SPEC-GAP-EXPECTED-FAIL` 提示不豁免任何失败，按实际逐项结果与退出码判定。

---

# 历史归档：KratosRagePatch 0.1.0 离线构建与验证记录

以下内容记录 2026-09-24 的 0.1.0 构建，其版本、测试数、前置、包清单和当时的 Git 状态均只属于历史。当前行为、依赖和交付说明以上文及 README 为准。

验证日期：2026-09-24。目标仅为 **Skyrim SE 1.6.640 / SKSE 2.2.3**。

**结论：当前源码的 Release DLL、ESP/SEQ、四个 PEX、HUD SWF 和配置已重新构建，以下离线检查全部通过。没有启动游戏，不能据此宣称游戏内验收通过。**

## 1. 本次接手续作

- 接手时已有 DLL 及配套资源，但 `docs/continuation-plan.md` 仍记载“尚无 DLL”；`dist` 为空，发行程序必需的本验证文档缺失。
- 先将接手时的 staging、README、续作计划及包测试文件保存到 `build/release-checks/before/`，再继续构建。原 MOD 基线继续使用 `build/resume-original-sha256.json`，没有重置基线。
- 复现了一项旧测试冲突：发行程序加入必需的 `SKSE/Plugins/StorageUtilData/KratosRage/config.json` 后，`test_no_stray_json_outside_the_oar_tree` 将它误报为杂文件。
- 仅调整这项测试：精确允许默认配置路径，并在配置存在时逐字节核对模板；目录归属使用路径边界判断，其他 JSON 仍被拒绝。未改动原生机制、Papyrus、ESP 生成规则或 HUD 源码。
- 补齐验证记录，并在 README 补充实际 DLL 所需的 Visual C++ x64 运行库、最低 Python 版本和完整记录分析命令。

## 2. 实际验证结果

下列日志为本次重新执行的结果，均位于项目的 `build/release-checks/`，不是直接沿用旧日志结论。

| 检查 | 本次结果 | 证据文件 |
| --- | --- | --- |
| 原生配置与 Release 构建 | DLL 与测试程序成功生成 | `native-configure.log`、`native-build.log` |
| CTest | 1/1 通过 | `ctest.log` |
| 怒气账本、变身事务、保存恢复 | 37 个注册用例、17 个命中夹具、10 个变身夹具；533 项断言，0 失败 | `rage-rules.log` |
| GameBridge mock 集成 | 19 个用例，190 项断言，0 失败 | `bridge-tests.log` |
| Papyrus 官方编译与汇编 | 4/4 成功，4 个 PEX 接口断言通过 | `papyrus.log` |
| Papyrus 第二前端交叉编译 | 4/4 成功；20 个配置字段与脚本默认值一致 | `papyrus.log` |
| AS2 / SWF | MTASC 1.17 编译通过；FWS v8、1280×720、30 fps、10 个标签；9 个 Papyrus 调用方法名通过交叉检查 | `ui.log` |
| Mutagen 插件验证 | 49/49 通过 | `../PluginBuilder/build-report.json` |
| 独立包结构与集成测试 | 53/53 通过，无跳过项 | `package-tests.log` |
| JSON 检查正反用例 | 5/5 符合预期 | `stage-layout-probes.json` |
| DLL 静态检查 | x64 PE；两个 SKSE 导出存在；staging DLL 与 Release DLL 逐字节一致 | `native-pe-inspection.log`、`toolchain.json` |
| 原 MOD 接手核对 | 1,013 个文件的路径及 SHA-256 与原基线一致 | `preflight.json` |

补充说明：

- GameBridge 测试原样复制实际 `GameBridge` / `RageState` 源码到隔离的构建目录，以 fake RE/SKSE 接口测试。它覆盖同帧任务队列、延迟确认、重复请求、读档后的旧任务隔离和重复 reconcile；不等于真实 Skyrim 事件时序已验证。
- 原测试输出末尾仍带有历史性的 `SPEC-GAP-EXPECTED-FAIL` 说明。当前对应的注册用例和夹具实际全部显示 `PASS`，没有豁免失败。
- JSON 正反探针只在内存中模拟文件枚举与配置内容，没有向 staging 注入杂文件。三个负向用例预期触发断言失败：额外 JSON、伪装为 OAR 路径前缀的兄弟目录、被修改的默认配置。探针日志中的这些 `FAIL` 是断言拒绝无效输入的证据；汇总结果均通过。加入合法默认配置、尚未加入默认配置的打包前状态均被接受；最终发行程序另行强制要求配置存在。
- PEX 内嵌编译时间，官方编译器与第二前端的序列化也不同，因此不要求两者二进制哈希一致。发行的是官方编译、汇编后的四个 PEX。

## 3. 插件与配置的检查边界

- ESP 为带 ESL 标记的 Form Version 44 插件，包含约定的 `0x800`–`0x806` 七条新记录；显式 masters 集合为 `Skyrim.esm` 和 `Dragonborn.esm`。Mutagen 实际写入顺序为 `Dragonborn.esm`、`Skyrim.esm`，相关 FormID 按这个文件内索引编码；此处列举依赖不表示运行时加载顺序。两个启动任务均写入 SEQ。
- VMAD 由 Mutagen 反解析，并由 Python 独立读取原始字节核对：任务对象属性的 alias 为 `-1`，玩家 alias 绑定到其所属任务的 alias `0`，脚本及对象属性与契约一致。
- OAR 生成三个原子组配置覆盖和三个独立 Ready 子组。Ready 子组保留原装备、Keytrace 等条件及原优先级，只追加满怒效果条件；通过 `overrideAnimationsFolder` 复用原动作。`SpartanRageTrigger2` 不覆盖，包内不复制 HKX。
- Payload Interpreter 的七条有效 `$SIBADA` 原载荷中，两条可施放的 Ability 载荷保持原字节，四条由请求法术及原生确认流程接管，一条错误类型的 MGEF 载荷不升级为额外效果。替换块之外的内容和注释逐字节保留；原生成功施放表不会再次执行保留的两条载荷。
- 特殊攻击白名单与原插件记录报告对齐；增益、切换武器、粉尘和持续区域伤害不纳入计分白名单。
- DLL 的版本声明及入口均限制为 1.6.640；静态导出检查不是在 SKSE 中成功加载的证明。实际依赖 `MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` 及 Windows 系统运行库，故安装前需具备 Visual C++ 2015–2022 Redistributable（x64）。

## 4. 工具链与可重建命令

本机实际使用：Windows 10 x64、Visual Studio 2022 Community / MSVC 14.44、Windows SDK 10.0.26100.0、.NET SDK 8.0.412、Python 3.14.3、项目内 CMake 3.31.8。

固定源码依赖为 CommonLibSSE-NG 3.7.0、fmt 11.2.0、spdlog 1.15.3、rapidcsv 8.84；下载地址及已记录的归档哈希见 `vendor/native-dependencies.json`。ESP 工具使用 Mutagen.Bethesda.Skyrim 0.53.1。Papyrus 使用项目内官方编译器/汇编器及第二前端；UI 使用项目内 MTASC 1.17。本次复用已下载的依赖和 NuGet 缓存，没有重新下载或安装系统组件。

以下为 **PowerShell** 命令，均以绝对项目路径定位，不需要切换当前目录。Python 最低为 3.11（发行程序使用 `hashlib.file_digest`）。原 MOD、游戏 Data 和脚本源目录只读；根据构建机器调整输入路径。`vendor` 中还需已有 Papyrus 编译器、SDK 头文件及 MTASC，原生依赖下载脚本不会下载这些工具。

### 4.1 原生依赖与 DLL

首次补齐原生依赖时使用前半段；本次依赖已安装在项目内，实际重新执行的是最后三条配置、构建和 CTest 命令。

```powershell
$root = 'D:\Project\KratosRagePatch'
$cmake = "$root\vendor\cmake\bin\cmake.exe"

python "$root\tools\fetch_native_dependencies.py"

& $cmake -S "$root\vendor\fmt" -B "$root\build\fmt" -G 'Visual Studio 17 2022' -A x64 -DFMT_TEST=OFF -DFMT_DOC=OFF -DFMT_INSTALL=ON -DBUILD_SHARED_LIBS=OFF "-DCMAKE_INSTALL_PREFIX=$root\vendor\prefix" '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL'
& $cmake --build "$root\build\fmt" --config Release --target install

& $cmake -S "$root\vendor\spdlog" -B "$root\build\spdlog" -G 'Visual Studio 17 2022' -A x64 -DSPDLOG_FMT_EXTERNAL=ON -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_INSTALL=ON "-DCMAKE_PREFIX_PATH=$root\vendor\prefix" "-DCMAKE_INSTALL_PREFIX=$root\vendor\prefix" '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL'
& $cmake --build "$root\build\spdlog" --config Release --target install

& $cmake -S $root -B "$root\build\native" -G 'Visual Studio 17 2022' -A x64 -DKR_BUILD_PLUGIN=ON -DBUILD_TESTING=ON
& $cmake --build "$root\build\native" --config Release --target KratosRage KratosRageTests
& "$root\vendor\cmake\bin\ctest.exe" --test-dir "$root\build\native" -C Release --output-on-failure
```

只对 fmt / spdlog 执行 `install`，安装前缀在项目内。不要对原生插件构建目录执行系统级安装；DLL 由 CMake 的构建后步骤复制到 `build/stage/SKSE/Plugins/`。

### 4.2 脚本、HUD、ESP 与配置

```powershell
$root = 'D:\Project\KratosRagePatch'
$gameData = 'F:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data'
$original = 'F:\Skyrim SE\ModOrganizer\mods\斧头God of War Kratos Combat MCO'

python "$root\tools\compile_papyrus.py" --game-source "$gameData\Scripts\Source"
python "$root\tools\build_ui.py"

dotnet run --project "$root\tools\PluginBuilder" --no-restore -c Release -- all --stage "$root\build\stage" --report "$root\build\PluginBuilder\record-report.json" --data $gameData --gow "$original\God of War.esp"
python "$root\tools\generate_oar.py" --mod $original --stage "$root\build\stage" --report "$root\build\PluginBuilder\oar-report.json"
```

`--no-restore` 仅适用于已有 NuGet 缓存的本次构建；首次构建时先恢复 `PluginBuilder.csproj` 的依赖。使用 `all` 而不是仅 `build`，以同时更新独立测试依赖的原记录分析报告。

### 4.3 离线测试与发行

```powershell
$root = 'D:\Project\KratosRagePatch'

& "$root\build\native\Release\KratosRageTests.exe" "$root\tests\fixtures"
python "$root\tools\test_bridge.py"
python "$root\tests\test_package.py"
python "$root\tools\package_release.py"
python "$root\tests\test_package.py"
python "$root\tools\verify_release.py"
```

发行程序在加入默认配置后要求 staging **恰好包含 16 个约定运行文件**，检查非空文件、ESL 标记、x64 DLL/SKSE 导出、PEX/SWF 文件头和插件验证报告，并重新核对原 MOD 的 1,013 个文件。它拒绝覆盖已有同名发行包；再次构建应使用 `--output` 指定新文件名，不要直接覆盖已交付包。

ZIP 为 MO2 Data-root 布局，加入 README、本验证记录、四份静态依赖许可证和清单后共 **23 个文件**。包内 `Docs/KratosRage/manifest.json` 为另外 22 个文件记录大小及 SHA-256，并明确 `gameRuntimeTested: false`；清单不对自身做递归哈希。发行程序会重新打开 ZIP 检查 CRC、文件集合和每个文件的字节内容。归档条目采用固定时间戳 `2026-09-23 00:00:00` 以固定打包元数据，不代表本次构建日期。

独立审计和五个布局正反探针已保存为 `tools/verify_release.py`。它只读取当前 staging、源码文档、ZIP 和原 MOD 基线，不修改安装目录或归档；可传 `--archive` 检查指定发行包。首次候选包已通过审计，并实测再次打包到同名路径会被拒绝且旧包哈希不变。正式发行路径为 `dist/KratosRagePatch-0.1.0-skyrim-1.6.640.zip`，整体 SHA-256 保存在旁边的同名 `.zip.sha256` 文件；最终独立审计记录位于 `build/release-checks/final-audit.json`。候选包及其旧日志保留在构建目录作为历史证据，不作为交付入口。

## 5. 未执行的验证与用户侧验收

本次没有安装到 MO2、修改 profile、启动游戏、操作桌面 UI 或改动原 MOD；工程没有 Git 仓库，本次未创建仓库、未提交 Git。

**以下仍需要用户使用备份测试存档验收：**

- SKSE 在目标版本中实际加载 DLL；SkyUI、PapyrusUtil、PO3、OAR 和 Payload Interpreter 在真实整合环境中的协作。
- 新档/旧档初始化、装备门槛、每种命中事件来源、击杀归属和密集攻击限速。
- 原变身操作是否在不足怒气时正常落回普攻并闪红，满怒后的动画、施放效果及原资源费用行为。
- 前摇中断与真实异步事件时序、存档加载后恢复、变身结束后的重新计分。
- HUD 的实际位置、缩放、渐隐/闪红和身体红金泛光外观。SWF 仅完成编译及结构检查，没有在 Scaleform 中渲染或进行视觉验收。

完整操作清单、前置与回退方式见 README。浏览器或 Android 模拟器不能替代 Skyrim / Scaleform 的运行时验证，本次没有把它们用于虚构游戏验收结果。
