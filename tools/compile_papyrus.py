#!/usr/bin/env python3
"""Compile the four KratosRage Papyrus scripts (papyrus/*.psc) into PEX files.

Self contained: every header/import source is merged into build/papyrus-headers/ first, so the
compile never depends on the active MO2 profile or on load order.

  build/papyrus-headers/    merged compile interfaces (headers only, never staged)
  build/papyrus/            KR_*.pex, primary (Bethesda PapyrusCompiler + PapyrusAssembler)
  build/papyrus/official/   raw official compiler output (audit trail)
  build/papyrus/crosscheck/ second front-end (open-source checker, see note below)
  build/papyrus/disasm/     PEX disassembly used for the interface assertions
  build/stage/Scripts/      staged PEX for packaging (exactly four files)

Note on the cross-check: vendor/papyrus-compiler also ships an independent .psc front-end
(its own parser/checker, which reports its own diagnostics). The two front-ends do NOT produce
byte identical PEX (different serialization and string table layout, and every PEX embeds a
compilation timestamp, so hashes change on every run); the second build is therefore a language
front-end cross-check only, while the interface assertions below are what actually pin the
compiled output. Verify by function/string assertions or disassembly, never by PEX hash.

Usage (from anywhere):
    python tools/compile_papyrus.py [--game-source DIR] [--check]

Exit code 0 only when all four PEX exist, the contract assertions on the compiled output hold,
and the staging directory holds exactly the four expected files.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]

SCRIPTS = ["KR_Native", "KR_Config", "KR_Widget", "KR_PlayerAlias"]

DEFAULT_GAME_SOURCE = Path(
    r"F:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\Scripts\Source"
)

COMPILER_DIR = PROJECT / "vendor" / "papyrus-compiler" / "papyrus-compiler"
OFFICIAL_COMPILER = COMPILER_DIR / "Original Compiler" / "PapyrusCompiler.exe"
OFFICIAL_FLAGS = COMPILER_DIR / "Original Compiler" / "TESV_Papyrus_Flags.flg"
CROSS_COMPILER = COMPILER_DIR / "papyrus.exe"

HEADERS = PROJECT / "build" / "papyrus-headers"
OUT = PROJECT / "build" / "papyrus"
OUT_OFFICIAL = OUT / "official"
OUT_CROSS = OUT / "crosscheck"
OUT_DISASM = OUT / "disasm"
STAGE = PROJECT / "build" / "stage" / "Scripts"

# Header sources, highest priority first; "first writer wins" so the game's own (SKSE modified)
# sources always shadow the plain vanilla ones.
HEADER_SOURCES = [
    ("game-source", DEFAULT_GAME_SOURCE),
    ("skse64-modified", PROJECT / "vendor" / "papyrus" / "skse64" / "modified"),
    ("skse64-vanilla", PROJECT / "vendor" / "papyrus" / "skse64" / "vanilla"),
    ("skyui-sdk", PROJECT / "vendor" / "skyui-sdk" / "Scripts" / "Source"),
    ("papyrus-extender", PROJECT / "vendor" / "papyrus" / "papyrus-extender"),
    ("papyrusutil", PROJECT / "vendor" / "papyrus" / "papyrusutil"),
    # Stubs only fill gaps (Debug.psc); they never override a real header.
    ("stubs", PROJECT / "vendor" / "papyrus" / "stubs"),
]

# Assertions against the disassembled primary PEX: the fixed interface from
# docs/implementation-contracts.md. A rename on either side breaks the build on purpose.
EXPECTED_FUNCTIONS = {
    "KR_Native": [
        "ConfigureRage", "SubmitWeaponHit", "SubmitMagicHit", "SubmitProjectileHit",
        "SubmitIncomingHit", "SubmitKill", "RefreshEquipmentState", "NotifyActivationAttempt",
        "ReconcileAfterLoad", "GetRage", "GetRageMaximum", "IsRaging", "IsReady", "IsEligible",
        "IsAvailable", "IsPending",
    ],
    "KR_Widget": [
        "OnInit", "OnGameReload", "OnWidgetInit", "OnWidgetReset", "GetWidgetSource",
        "GetWidgetType", "GetDimensions", "ApplySettings", "Initialize", "UpdateRage",
        "RefreshVisibility", "ShowTemporarily", "FlashDenied", "OnUpdate", "ComputeWantVisible",
        "ArmCheck",
    ],
    "KR_Config": [
        "LoadConfiguration", "HandlePlayerLoad", "RegisterCallbacks", "OnKrRageChanged",
        "OnKrRageDenied", "OnKrRageBecameFull", "ReadInt", "ReadFloat", "ReadBool", "ReadColor",
        "AllowFullFeedback", "OnUpdate",
    ],
    "KR_PlayerAlias": [
        "EnsureBridge", "RegisterPoiEvents", "RegisterInputControls", "RegisterRageEvents",
        "IsControlPressed", "RefreshEquipment", "OnWeaponHit", "OnMagicHit", "OnProjectileHit",
        "OnHitEx", "OnActorKilled", "OnRageBecameFull", "OnRageChanged", "ArmRageWatch", "OnUpdate",
        "OnObjectEquipped", "OnObjectUnequipped", "OnCombatStateChanged", "OnControlDown",
        "OnPlayerLoadGame",
    ],
}

EXPECTED_STRINGS = {
    # SWF method names the widget drives through UI.Invoke - these must match ui/src/KratosRageMeter.as
    "KR_Widget": [
        "KratosRage/meter.swf", ".setPercent", ".setReady", ".setStyle", ".showMeter",
        ".hideMeter", ".flashDenied", ".fadeTo", ".setAlpha", ".cancelAnimations",
        # OnInit writes the inherited SKI_WidgetBase auto property, which lives in the parent PEX:
        # the derived PEX must reference its backing variable, not declare a new property.
        "::RequireExtend_var",
    ],
    # native -> Papyrus callback names, the JSON file, and the resolved glow feedback
    "KR_Config": [
        "KR_RageChanged", "KR_RageDenied", "KR_RageBecameFull", "KratosRage/config",
        "feedback.fullGlowEnabled", "feedback.fullGlowDuration",
        # the configuration is read through the JsonUtil path API; the plain getters only see
        # JsonUtil's own "int"/"float"/"string"/"form" layout and would silently return the fallback
        # for this nested, hand-authored file
        "ident(GetPathIntValue)", "ident(GetPathFloatValue)", "ident(GetPathBoolValue)",
        # ... and the file is re-read on every configuration push instead of trusting a cached copy
        "ident(Load)", "ident(JsonExists)",
    ],
    # Input observation and effect shader feedback; PO3 is not a diagnostic dependency.
    "KR_PlayerAlias": [
        "RegisterForControl",
        "Right Attack/Block", "GetMappedKey", "IsKeyPressed",
        # the alias, not the native DLL, plays the 0x806 glow; only its own shader may be touched
        "KR_RageBecameFull", "OnGameReload", "AllowFullFeedback", "ident(Stop)", "ident(Play)",
        # end-of-rage watch: exactly one single-shot update while a rage is active, or while a
        # started transformation is still unconfirmed
        "RefreshEquipmentState", "RegisterForSingleUpdate", "IsRaging", "IsPending",
    ],
}

# Strings that must be absent from the compiled PEX. The configuration lives in a nested,
# hand-authored JSON file, and the plain JsonUtil getters resolve keys inside JsonUtil's own
# "int"/"float"/"string"/"form" containers only: on such a file they silently return the fallback for
# every field. A revert to them has to break the build instead of shipping a configuration whose
# values can never be changed.
EXPECTED_ABSENT_STRINGS = {
    "KR_Widget": [".setValue", "_rageCurrent", "_rageMaximum"],
    "KR_Config": ["GetIntValue", "GetFloatValue", "GetIntValue2", "GetFloatValue2", "GetBoolValue"],
    "KR_PlayerAlias": ["PO3_Events_Alias", "RegisterForWeaponHit", "RegisterForMagicHit",
                       "RegisterForProjectileHit", "RegisterForHitEventEx", "RegisterForActorKilled"],
}


def log(message: str) -> None:
    print(message, flush=True)


# data/config.default.json key path -> KR_Config.psc default property. The shipped template and the
# in-script fallbacks have to agree, otherwise a missing file would behave differently from one
# holding the documented defaults.
CONFIG_MAP = {
    "rage.maximum": "DefaultMaximum",
    "rage.gain.normalHit": "DefaultNormalHit",
    "rage.gain.powerHit": "DefaultPowerHit",
    "rage.gain.specialHit": "DefaultSpecialHit",
    "rage.gain.receivedHit": "DefaultReceivedHit",
    "rage.gain.kill": "DefaultKill",
    "rage.maxGainPerSecond": "DefaultMaxGainPerSecond",
    "hud.x": "DefaultHudX",
    "hud.y": "DefaultHudY",
    "hud.width": "DefaultHudWidth",
    "hud.height": "DefaultHudHeight",
    "hud.fillColor": "DefaultFillColor",
    "hud.readyColor": "DefaultReadyColor",
    "hud.alpha": "DefaultHudAlpha",
    "hud.outOfCombatDelay": "DefaultOutOfCombatDelay",
    "hud.fadeDuration": "DefaultFadeDuration",
    "feedback.deniedFlashDuration": "DefaultDeniedFlashDuration",
    "feedback.deniedInterval": "DefaultDeniedInterval",
    "feedback.fullGlowEnabled": "DefaultFullGlowEnabled",
    "feedback.fullGlowDuration": "DefaultFullGlowDuration",
}


def flatten(prefix: str, value, out: dict) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            flatten(f"{prefix}.{key}" if prefix else key, item, out)
    else:
        out[prefix] = value


def check_default_config() -> list[str]:
    problems: list[str] = []
    config_path = PROJECT / "data" / "config.default.json"
    script_path = PROJECT / "papyrus" / "KR_Config.psc"
    if not config_path.is_file():
        return [f"missing {config_path}"]
    if not script_path.is_file():
        return [f"missing {script_path}"]

    flat: dict = {}
    try:
        flatten("", json.loads(config_path.read_text(encoding="utf-8")), flat)
    except json.JSONDecodeError as exc:
        return [f"{config_path} is not valid JSON: {exc}"]

    for key in CONFIG_MAP:
        if key not in flat:
            problems.append(f"config.default.json is missing {key}")
    for key in flat:
        if key not in CONFIG_MAP:
            problems.append(f"config.default.json has undocumented key {key}")

    source = script_path.read_text(encoding="utf-8")
    for key, prop in CONFIG_MAP.items():
        match = re.search(rf"^\s*\w+ Property {prop} = (\S+) AutoReadOnly", source, re.MULTILINE)
        if match is None:
            problems.append(f"KR_Config.psc is missing property {prop}")
            continue
        if key not in flat:
            continue
        declared = match.group(1)
        expected = flat[key]
        if isinstance(expected, bool):
            declared_bool = declared.lower() == "true"
            if declared_bool != expected:
                problems.append(f"{prop} = {declared} but config.default.json has {expected}")
        else:
            try:
                if float(declared) != float(expected):
                    problems.append(f"{prop} = {declared} but config.default.json has {expected}")
            except ValueError:
                problems.append(f"{prop} = {declared} is not numeric")

    # The reader call sites are the other half of the fixture: each one names the JSON key it reads
    # and the property it falls back to. A typo there would silently serve the default value for that
    # field (the read cannot fail loudly), so the pairing is checked against the shipped file here.
    call_sites = re.findall(
        r"\b(?:ReadInt|ReadFloat|ReadBool|ReadColor)\(\s*\"([^\"]+)\"\s*,\s*(\w+)", source)
    if not call_sites:
        problems.append("KR_Config.psc has no field reader call sites")
    for key, prop in call_sites:
        if key not in CONFIG_MAP:
            problems.append(f"KR_Config.psc reads undocumented key {key!r}")
        elif CONFIG_MAP[key] != prop:
            problems.append(
                f"KR_Config.psc reads {key!r} with fallback {prop}, expected {CONFIG_MAP[key]}")
    for key in CONFIG_MAP:
        if not any(read_key == key for read_key, _ in call_sites):
            problems.append(f"KR_Config.psc never reads {key!r}")
    return problems


def win_path(path: Path) -> str:
    """The .NET based official compiler needs backslash paths."""
    return str(path).replace("/", "\\")


def run(cmd: list[str]) -> tuple[int, str]:
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    return proc.returncode, proc.stdout


def merge_headers(game_source: Path) -> Path:
    if HEADERS.exists():
        shutil.rmtree(HEADERS)
    HEADERS.mkdir(parents=True, exist_ok=True)

    sources = list(HEADER_SOURCES)
    sources[0] = ("game-source", game_source)

    for name, directory in sources:
        if not directory.is_dir():
            log(f"  ! header source missing, skipped: {name}: {directory}")
            continue
        copied = 0
        for psc in sorted(directory.glob("*.psc")):
            target = HEADERS / psc.name
            if target.exists():
                continue
            shutil.copy2(psc, target)
            copied += 1
        log(f"  + {name}: {copied} new header(s) from {directory}")
    log(f"  = {len(list(HEADERS.glob('*.psc')))} headers in {HEADERS}")
    return HEADERS


def compile_official() -> bool:
    """Bethesda's PapyrusCompiler.exe is a front-end: it needs PapyrusAssembler.exe next to it,
    which is why -noasm must NOT be passed."""
    if not OFFICIAL_COMPILER.is_file():
        log("  ! official compiler not found")
        return False
    if OUT_OFFICIAL.exists():
        shutil.rmtree(OUT_OFFICIAL)
    OUT_OFFICIAL.mkdir(parents=True, exist_ok=True)

    imports = ";".join(win_path(p) for p in (HEADERS, PROJECT / "papyrus"))
    code, output = run([
        str(OFFICIAL_COMPILER),
        win_path(PROJECT / "papyrus"),
        "-all",
        f"-i={imports}",
        f"-f={win_path(OFFICIAL_FLAGS)}",
        f"-o={win_path(OUT_OFFICIAL)}",
        "-quiet",
    ])
    log(output.rstrip())
    return code == 0


def compile_cross() -> bool:
    if not CROSS_COMPILER.is_file():
        log("  ! cross-check front-end not found, skipped")
        return False
    if OUT_CROSS.exists():
        shutil.rmtree(OUT_CROSS)
    OUT_CROSS.mkdir(parents=True, exist_ok=True)

    code, output = run([
        str(CROSS_COMPILER), "compile", "-nocache",
        "-h", str(HEADERS),
        "-i", str(PROJECT / "papyrus"),
        "-o", str(OUT_CROSS),
    ])
    log(output.rstrip())
    return code == 0


def disassemble(pex: Path, out_file: Path) -> str | None:
    code, output = run([str(CROSS_COMPILER), "read", str(pex)])
    if code != 0:
        log(f"  ! disassembly failed for {pex.name}: {output.strip()[:200]}")
        return None
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(output, encoding="utf-8")
    return output


def collect(directory: Path) -> dict[str, Path]:
    return {name: directory / f"{name}.pex" for name in SCRIPTS if (directory / f"{name}.pex").is_file()}


def check_interface(name: str, text: str) -> list[str]:
    problems: list[str] = []
    for func in EXPECTED_FUNCTIONS[name]:
        if f"func name: '{func}'" not in text:
            problems.append(f"missing function {func}")
    for needle in EXPECTED_STRINGS.get(name, []):
        if needle not in text:
            problems.append(f"missing string {needle!r}")
    for needle in EXPECTED_ABSENT_STRINGS.get(name, []):
        if needle in text:
            problems.append(f"forbidden string {needle!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-source", type=Path, default=DEFAULT_GAME_SOURCE)
    parser.add_argument("--check", action="store_true", help="compile and verify only, skip staging")
    args = parser.parse_args()

    log("[1/6] verifying data/config.default.json against KR_Config.psc")
    config_problems = check_default_config()
    for problem in config_problems:
        log(f"  ! {problem}")
    if config_problems:
        log("ERROR: default configuration and script defaults disagree")
        return 2
    log(f"  {len(CONFIG_MAP)} documented fields match the in-script defaults")

    log("[2/6] merging compile headers")
    headers = merge_headers(args.game_source)

    log("[3/6] compiling with the official Papyrus compiler")
    official_ok = compile_official()
    if not official_ok:
        log("ERROR: official compiler failed")
        return 2
    produced = collect(OUT_OFFICIAL)
    missing = [name for name in SCRIPTS if name not in produced]
    if missing:
        log(f"ERROR: official compiler produced no PEX for: {', '.join(missing)}")
        return 2
    log("  = all four scripts compiled and assembled")

    # Promote the official output to the primary artifact set (stale PEX are removed explicitly,
    # the directory itself keeps the audit sub-directories).
    for stale in OUT.glob("*.pex"):
        stale.unlink()
    for name, path in produced.items():
        shutil.copy2(path, OUT / path.name)

    log("[4/6] cross-check compiling with the second front-end")
    cross_ok = compile_cross()
    cross_produced = collect(OUT_CROSS) if cross_ok else {}
    for name in SCRIPTS:
        primary_size = (OUT / f"{name}.pex").stat().st_size
        cross_size = cross_produced[name].stat().st_size if name in cross_produced else 0
        log(f"  {name}: primary {primary_size} B, second front-end {cross_size} B")

    log("[5/6] disassembling and asserting the fixed interface")
    failed = False
    for name in SCRIPTS:
        text = disassemble(OUT / f"{name}.pex", OUT_DISASM / f"{name}.txt")
        if text is None:
            failed = True
            continue
        problems = check_interface(name, text)
        if problems:
            failed = True
            for problem in problems:
                log(f"  ! {name}.pex: {problem}")
        else:
            log(f"  {name}.pex: interface assertions hold")
    if failed:
        log("ERROR: compiled output does not match the fixed interface")
        return 3

    if args.check:
        log("[6/6] --check: staging skipped")
        return 0

    log("[6/6] staging")
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True, exist_ok=True)
    for name in SCRIPTS:
        shutil.copy2(OUT / f"{name}.pex", STAGE / f"{name}.pex")

    staged = sorted(p.name for p in STAGE.glob("*"))
    expected = sorted(f"{name}.pex" for name in SCRIPTS)
    if staged != expected:
        log(f"ERROR: staging directory holds unexpected files: {staged}")
        return 4

    for name in SCRIPTS:
        pex = STAGE / f"{name}.pex"
        log(f"  staged {pex} ({pex.stat().st_size} B)")

    log("build/papyrus OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
