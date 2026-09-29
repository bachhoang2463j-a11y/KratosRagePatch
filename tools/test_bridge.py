#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KratosRagePatch —— GameBridge 离线 mock 集成测试驱动。

做什么
------
1. 把 src/GameBridge.{h,cpp} 与 src/RageState.{h,cpp} **原样**复制到
   build/bridge-tests/source/（逐字节校验），并写入同目录的 fake PCH.h
   （来自 tests/bridge/fake/PCH.h，顶替真实 RE/SKSE/spdlog 头）。
2. 用工程内 vendor/cmake 的 CMake + VS2022 Community 编译
   tests/bridge/BridgeTests.cpp + 上述副本，产物与日志都留在 build/bridge-tests/。
3. 运行测试可执行文件并回传退出码。

边界
----
* 只写 tests/bridge/**、tools/test_bridge.py、build/bridge-tests/**；
  绝不修改 src/、根 CMakeLists、vendor/、其它 tools/、原 MOD。
* 这是 **mock 集成**：断言的是「真实 GameBridge 逻辑在当前源码下的行为」，
  不是游戏内验证；伪 API 见 tests/bridge/fake/Fake.h 顶部说明。

变异体验证（证明用例真的能抓旧时序缺陷）
----------------------------------------
  --mutant retry    把确认逻辑改成「同帧立刻回滚」，预期的旧行为 →
                    pending_survives_same_frame_drain 必须 FAIL。
  --mutant noepoch  去掉 Queue 的 generation 守卫（旧的无 epoch 实现）→
                    旧 Configure 泄漏与对账完成后执行旧命中的 Restore/Revert 用例必须 FAIL。
变异只作用于复制的副本，src/ 始终保持原样。用例只在 --mutant none 下要求全绿。

用法
----
  python tools/test_bridge.py
  python tools/test_bridge.py --mutant retry
  python tools/test_bridge.py --no-build
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT / "src"
TEST_DIR = ROOT / "tests" / "bridge"
FAKE_DIR = TEST_DIR / "fake"
WORK_DIR = ROOT / "build" / "bridge-tests"
COPY_DIR = WORK_DIR / "source"
BUILD_DIR = WORK_DIR / "build"
LOG_DIR = WORK_DIR / "logs"

COPY_FILES = ("GameBridge.h", "GameBridge.cpp", "RageState.h", "RageState.cpp")
PCH_SOURCE = FAKE_DIR / "PCH.h"
TARGET_NAME = "KratosRageBridgeTests"

# (pattern, replacement, expected_matches) —— 只打在副本上，用于证明用例的区分能力
MUTANTS = {
    "none": (),
    "retry": (
        (
            r"else if \(Now\(\) >= activationDeadline_\) \{",
            "else if (true) { /* mutant: 同帧立即回滚（旧行为） */",
            1,
        ),
    ),
    "noepoch": (
        (
            r"if \(generation == generation_\) \{\s*\n\s*task\(\);\s*\n\s*\}",
            "(void)generation; /* mutant: 无 epoch */ task();",
            1,
        ),
    ),
    "reconcile": (
        (
            r"if \(!needsReconcile_\) \{\s*\n\s*return;\s*\n\s*\}",
            "if (false) { /* mutant: 去掉重复 reconcile 守卫 */ return; }",
            1,
        ),
    ),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def find_cmake() -> str:
    vendored = ROOT / "vendor" / "cmake" / "bin" / "cmake.exe"
    if vendored.is_file():
        return str(vendored)
    found = shutil.which("cmake")
    if found:
        return found
    raise SystemExit("找不到 cmake（vendor/cmake/bin/cmake.exe 与 PATH 都没有）")


def run(cmd: list[str], log_path: Path, label: str) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"$ {' '.join(cmd)}")
    with log_path.open("w", encoding="utf-8", errors="replace") as handle:
        handle.write(f"# {label}\n$ {' '.join(cmd)}\n\n")
        handle.flush()
        try:
            completed = subprocess.run(cmd, stdout=handle, stderr=subprocess.STDOUT, cwd=str(ROOT), timeout=180)
        except subprocess.TimeoutExpired:
            handle.write("\nERROR: command exceeded 180 seconds\n")
            return 124
    return completed.returncode


def tail(log_path: Path, lines: int = 40) -> None:
    if not log_path.is_file():
        return
    content = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    print(f"--- {log_path.relative_to(ROOT)} (tail {lines}) ---")
    for line in content[-lines:]:
        print(line)


def stage_sources(mutant: str) -> None:
    # 硬护栏：本脚本绝不写 src/。副本目录必须落在 build/bridge-tests 内且与 src 无包含关系。
    src_root = SRC_DIR.resolve()
    copy_root = COPY_DIR.resolve()
    if copy_root == src_root or src_root in copy_root.parents or copy_root in src_root.parents:
        raise SystemExit(f"拒绝执行：副本目录 {copy_root} 与 src 目录 {src_root} 关系不合法")
    if copy_root.parent != WORK_DIR.resolve():
        raise SystemExit(f"拒绝执行：副本目录 {copy_root} 必须直接位于 {WORK_DIR} 下")

    COPY_DIR.mkdir(parents=True, exist_ok=True)
    for name in COPY_FILES:
        shutil.copyfile(SRC_DIR / name, COPY_DIR / name)
    shutil.copyfile(PCH_SOURCE, COPY_DIR / "PCH.h")

    # 逐字节校验：副本必须与 src 完全一致（fake PCH.h 除外，它本来就是要替换的）
    for name in COPY_FILES:
        if sha256(SRC_DIR / name) != sha256(COPY_DIR / name):
            raise SystemExit(f"复制校验失败：{name} 副本与 src 不一致")

    if mutant != "none":
        target = COPY_DIR / "GameBridge.cpp"  # 只改副本，src 永不触碰
        text = target.read_text(encoding="utf-8")
        for pattern, replacement, expected in MUTANTS[mutant]:
            text, count = re.subn(pattern, replacement, text)
            if count != expected:
                raise SystemExit(
                    f"变异 {mutant} 未生效：模式 {pattern!r} 命中 {count} 次（期望 {expected}）。"
                    "GameBridge 可能又改了，请同步 tools/test_bridge.py 的变异模式。"
                )
        target.write_text(text, encoding="utf-8")
        print(f"!! 已对副本施加变异：{mutant}（仅 build/bridge-tests/source，src/ 未动）")


def main() -> int:
    parser = argparse.ArgumentParser(description="GameBridge 离线 mock 集成测试")
    parser.add_argument("--mutant", choices=sorted(MUTANTS), default="none",
                        help="对副本施加旧实现变异，用于验证用例的区分能力")
    parser.add_argument("--no-build", action="store_true", help="跳过配置与编译，直接跑已有可执行文件")
    parser.add_argument("--clean", action="store_true", help="先删除 build/bridge-tests 再开始")
    parser.add_argument("--config", default="Release", help="CMake 配置（默认 Release）")
    args = parser.parse_args()

    if args.clean and WORK_DIR.exists():
        shutil.rmtree(WORK_DIR)
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    exe = BUILD_DIR / args.config / f"{TARGET_NAME}.exe"
    src_before = {name: sha256(SRC_DIR / name) for name in COPY_FILES}

    if not args.no_build:
        stage_sources(args.mutant)

        print("\n-- 被测源码指纹（drift 检测用） --")
        for name in COPY_FILES:
            print(f"  {name}: {src_before[name]}")

        cmake = find_cmake()
        print()
        configure = run(
            [cmake, "-S", str(TEST_DIR), "-B", str(BUILD_DIR), "-G", "Visual Studio 17 2022", "-A", "x64"],
            LOG_DIR / "configure.log",
            "cmake configure",
        )
        if configure != 0:
            print(f"\nconfigure 失败，exit={configure}")
            tail(LOG_DIR / "configure.log")
            return configure

        build = run(
            [cmake, "--build", str(BUILD_DIR), "--config", args.config, "--target", TARGET_NAME],
            LOG_DIR / "build.log",
            "cmake build",
        )
        print(f"build exit={build}")
        if build != 0:
            tail(LOG_DIR / "build.log")
            return build

    if not exe.is_file():
        raise SystemExit(f"找不到测试可执行文件 {exe}（先去掉 --no-build）")

    code = run([str(exe)], LOG_DIR / "run.log", "run tests")
    tail(LOG_DIR / "run.log", 80)

    # 并发写检测：运行期间若 src 被别的写入方改动，本次结果只对应上面的 pre 指纹。
    src_after = {name: sha256(SRC_DIR / name) for name in COPY_FILES}
    drifted = [name for name in COPY_FILES if src_before[name] != src_after[name]]
    if drifted:
        print("\n!! 本次运行期间 src 被外部写入：" + ", ".join(drifted))
        print("!! 上面的编译/测试结果对应运行开始时打印的 pre 指纹，请重跑一次取一致快照。")

    print(f"\nrun exit={code}  (log: {(LOG_DIR / 'run.log').relative_to(ROOT)})")
    if args.mutant != "none":
        print(f"note: mutant={args.mutant} 下用例应当 FAIL（用于证明回归用例能抓旧实现）")
    return code


if __name__ == "__main__":
    sys.exit(main())
