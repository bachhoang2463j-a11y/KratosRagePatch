# KratosRagePatch 0.1.0 离线构建与验证记录

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
