#!/usr/bin/env python3
"""Generate the OAR + PayloadInterpreter part of the KratosRagePatch staging folder.

Reads the *original* mod (read-only) and writes only into the patch stage:

  <stage>/meshes/actors/character/animations/OpenAnimationReplacer/KratosMoveset/
      SpartanRageTrigger/config.json          overlay: attack1-10 disabled
      Barehanded/config.json                  overlay: mco_weaponart disabled
      LeviathanAxe/config.json                overlay: mco_weaponart disabled
      SpartanRageTriggerReady/config.json     new: original conditions + 0x803, animations reused
      BarehandedReady/config.json             new: original conditions + 0x803, animations reused
      LeviathanAxeReady/config.json           new: original conditions + 0x803, animations reused
  <stage>/SKSE/PayloadInterpreter/Config/God of War4.ini
      overlay: the request cast 0x804 is added, the castable Ability payloads are kept verbatim,
      every other payload and comment is left byte-identical.

The overlays are whole-file replacements that win the MO2 file conflict, so the original mod is
never edited; every untouched line/byte is copied verbatim.

Each *Ready* group is a separate submod, one per equipment set, and keeps its source submod's
conditions exactly as they are (equipment, Keytrace hotkeys, the negated God of War.esp 0x813 rage
gate) and only appends the patch's ready marker as one more AND condition. It carries no animation
of its own: "overrideAnimationsFolder" points at the sibling folder of its source submod, so the
original HKX files are reused and nothing is copied.

Format verified against the OpenAnimationReplacer 2.3.4 sources in build/refs/OAR-2.3.4
(git HEAD 6b1fc1e "Version 2.3.4", CMakeLists.txt VERSION 2.3.4):
  * src/OpenAnimationReplacer.cpp:491  - the scan root is the literal "data\\meshes\\", so an
    animation's runtime path is "data\\meshes\\" + the real on-disk path under meshes/.
  * src/Parsing.h:106, src/Parsing.cpp:314-317 - "overrideAnimationsFolder" is a supported
    SubModParseResult member read from the submod's config.json.
  * src/Parsing.cpp:749-761            - resolved as <submod folder>/.. / <value>; if that is a
    directory the submod loads its animations from it instead of its own folder, otherwise the
    submod fails to load. "SpartanRageTrigger" therefore means the sibling folder inside the same
    replacer mod: HKX reused, never copied. The option is also exposed in OAR's own UI
    (src/UI/UIMain.cpp:1141-1162, tooltip "this submod will load animations from the given folder
    (in the parent directory) instead of its own folder") and is used by installed mods, e.g.
    "1-Elden Rim-Weapon Art 2.3.2/.../WarAsh/000ActionSurge/config.json".
  * src/Parsing.cpp:239-308            - "replacementAnimDatas" is an array of objects carrying
    projectName + path (+ disabled).
  * src/ReplacerMods.cpp:70,440-540    - matching is exact string equality on projectName and on
    the animation's full runtime path; OAR itself only writes projectName/path/disabled for an
    entry without variants. projectName is the behaviour project (DefaultMale / DefaultFemale).
  * src/ReplacementAnimation.cpp:279   - a disabled replacement fails its condition check, so the
    entry is simply not offered by this submod.
  * src/Parsing.cpp:706-760            - submods are keyed by their folder path, so an external
    patch can only influence the original submods by overriding their config.json file.

Nothing here writes to the original mod, the game install or the MO2 profile.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:  # keep the console readable on a GBK/cp1252 code page
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover - older interpreters
    pass

REPO_ROOT = Path(r"D:\Project\KratosRagePatch")
DEFAULT_MOD = Path(r"F:\Skyrim SE\ModOrganizer\mods\斧头God of War Kratos Combat MCO")
DEFAULT_STAGE = REPO_ROOT / "build" / "stage"
DEFAULT_REPORT = REPO_ROOT / "build" / "PluginBuilder" / "oar-report.json"

PATCH_PLUGIN = "KratosRagePatch.esp"
READY_EFFECT_ID = 0x803  # MGEF KR_RageReadyEffect - the "rage is ready" marker

OAR_DIR_PARTS = ("meshes", "actors", "character", "animations", "OpenAnimationReplacer")
REPLACER_MOD = "KratosMoveset"
RUNTIME_PREFIX = "data\\meshes\\"

# Behaviour projects whose animation bundles contain the mco_* animations this moveset replaces.
# OAR matches replacementAnimDatas by exact project name; the mod ships one flat set of HKX files
# (no male/female split), so both player projects can load them.
PROJECT_NAMES = ("DefaultMale", "DefaultFemale")

PI_INI_REL = Path("SKSE") / "PayloadInterpreter" / "Config" / "God of War4.ini"
# The request cast the native side listens for (TESMagicEffectApplyEvent on MGEF 0x805).
PI_REQUEST_LINE = b"$SIBADA = @CAST|0x804|KratosRagePatch.esp|1|1|1|0|0|0|0|0|0"
# Payloads that are castable and that the plan keeps verbatim: PI's @CAST maps to
# CastSpellImmediate, which applies Ability-type spells too, so 0x817 and 0x844 are *not* dead
# casts. Their parameters must not be dropped or repurposed.
PI_KEEP_FORM_TOKENS = ("0X817", "0X844")
# Only a payload whose form is not a spell at all can never be applied (it is a MGEF).
PI_DROP_FORM_TOKENS = ("0X201DF97",)

# submod folder -> the animations this patch disables inside it
DISABLE_MCO_ATTACK = [f"mco_attack{i}" for i in range(1, 11)]
DISABLES = {
    "SpartanRageTrigger": DISABLE_MCO_ATTACK,
    "Barehanded": ["mco_weaponart"],
    "LeviathanAxe": ["mco_weaponart"],
}

READY_SUFFIX = "Ready"           # new submod folder = <source submod> + this suffix
UNTOUCHED_SUBMODS = ("SpartanRageTrigger2",)  # explicitly not locked/modified

READY_GATE = {
    "condition": "HasMagicEffect",
    "requiredVersion": "1.0.0.0",
    "Magic effect": {"pluginName": PATCH_PLUGIN, "formID": f"{READY_EFFECT_ID:X}"},
    "Active effects only": False,
}

CONFIG_ORDER = ("name", "description", "priority", "disabled", "replacementAnimDatas",
                "overrideAnimationsFolder", "requiredProjectName")


class GeneratorError(Exception):
    pass


# --------------------------------------------------------------------------- helpers

def ci_child(parent: Path, name: str) -> Path:
    """Return the real child of `parent` whose name matches `name` case-insensitively."""
    if not parent.is_dir():
        raise GeneratorError(f"not a directory: {parent}")
    matches = [e for e in parent.iterdir() if e.name.lower() == name.lower()]
    if len(matches) != 1:
        raise GeneratorError(f"expected exactly one '{name}' inside {parent}, found {len(matches)}")
    return matches[0]


def resolve_below(root: Path, parts) -> Path:
    """Walk `parts` below `root`, preserving the on-disk spelling of every component."""
    current = root
    for part in parts:
        current = ci_child(current, part)
    return current


def runtime_path(mod_root: Path, file_path: Path) -> str:
    """The string OAR builds for an animation: 'data\\meshes\\' + the real on-disk remainder."""
    rel = file_path.relative_to(resolve_below(mod_root, ("meshes",)))
    return RUNTIME_PREFIX + str(rel).replace("/", "\\")


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def dump_json(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(document, ensure_ascii=False, indent=4) + "\n"
    path.write_text(text, encoding="utf-8", newline="\n")


def ordered_config(document: dict) -> dict:
    """Rebuild a config in OAR's own writer order (extra keys keep their original position)."""
    result = {}
    for key in CONFIG_ORDER:
        if key in document:
            result[key] = document[key]
    for key, value in document.items():
        if key not in result:
            result[key] = value
    return result


def find_animations(folder: Path) -> dict:
    """Map lowercase animation stem -> real file names for every .hkx directly in `folder`."""
    if not folder.is_dir():
        raise GeneratorError(f"missing animation folder: {folder}")
    found = {}
    for entry in folder.iterdir():
        if not entry.is_file() or entry.suffix.lower() != ".hkx":
            continue
        found.setdefault(entry.stem.lower(), []).append(entry.name)
    return found


# --------------------------------------------------------------------------- OAR overlays

def build_override(submod: str, animations: list, mod_root: Path, oar_folder: Path,
                   stage_oar: Path) -> dict:
    """Write the overlay config.json for one original submod, disabling the given animations."""
    source_dir = ci_child(oar_folder, submod)
    config_path = source_dir / "config.json"
    if not config_path.is_file():
        raise GeneratorError(f"missing config.json: {config_path}")

    document = load_json(config_path)
    kept = list(document.get("replacementAnimDatas", []))
    entries = []

    for stem in animations:
        matches = find_animations(source_dir).get(stem)
        if matches is None:
            raise GeneratorError(f"{submod}: animation '{stem}.hkx' not found in {source_dir}")
        if len(matches) > 1:
            raise GeneratorError(f"{submod}: ambiguous animation '{stem}': {matches}")
        file_path = source_dir / matches[0]
        for project in PROJECT_NAMES:
            entries.append({
                "projectName": project,
                "path": runtime_path(mod_root, file_path),
                "disabled": True,
            })

    new_paths = {(e["projectName"], e["path"]) for e in entries}
    kept = [e for e in kept if (e.get("projectName"), e.get("path")) not in new_paths]
    document["replacementAnimDatas"] = kept + entries
    document = ordered_config(document)

    target = stage_oar / REPLACER_MOD / submod / "config.json"
    dump_json(target, document)

    disabled_files = []
    for entry in entries:
        name = entry["path"].rsplit("\\", 1)[-1]
        if name not in disabled_files:
            disabled_files.append(name)

    return {
        "submod": submod,
        "sourceConfig": str(config_path),
        "sourceBytes": config_path.stat().st_size,
        "target": str(target),
        "disabledAnimations": animations,
        "disabledFiles": disabled_files,
        "replacementAnimDatasWritten": len(entries),
        "projects": list(PROJECT_NAMES),
        "conditionsUnchanged": True,
    }


def build_ready_submod(source: str, oar_folder: Path, stage_oar: Path) -> dict:
    """Write one <source>Ready submod: the source conditions verbatim, plus the 0x803 gate."""
    source_dir = ci_child(oar_folder, source)
    source_config = load_json(source_dir / "config.json")
    conditions = list(source_config.get("conditions", []))
    if not conditions:
        raise GeneratorError(f"{source}: no conditions to keep")

    for condition in conditions:
        if condition.get("condition") == "HasMagicEffect" and \
                (condition.get("Magic effect") or {}).get("pluginName") == PATCH_PLUGIN:
            raise GeneratorError(f"{source}: the 0x803 gate is already present in the original")

    name = f"{source}{READY_SUFFIX}"
    document = ordered_config({
        "name": name,
        "description": (f"KratosRagePatch: {source} with the rage-ready gate. Keeps every original "
                        f"condition of {source} (equipment, Keytrace hotkeys, negated God of "
                        f"War.esp 0x813) and only adds HasMagicEffect {PATCH_PLUGIN} "
                        f"0x{READY_EFFECT_ID:X}. Animations are reused from the sibling folder "
                        f"'{source}' - no HKX is copied."),
        "priority": source_config["priority"],
        "overrideAnimationsFolder": source,
        "conditions": conditions + [READY_GATE],
    })

    target = stage_oar / REPLACER_MOD / name / "config.json"
    dump_json(target, document)

    return {
        "submod": name,
        "sourceSubmod": source,
        "target": str(target),
        "priority": source_config["priority"],
        "prioritySource": f"{source}/config.json",
        "overrideAnimationsFolder": source,
        "reusedFolder": str(source_dir),
        "hkxCopied": 0,
        "sourceConditionsKept": len(conditions),
        "appendedCondition": READY_GATE,
    }


def oar_files(root: Path) -> list:
    return sorted(str(p.relative_to(root)).replace("\\", "/")
                  for p in root.rglob("*") if p.is_file())


# --------------------------------------------------------------------------- PI ini

def payload_form_token(line: bytes) -> str:
    """The form id token of a payload line, e.g. '@CAST|0X817|God of War.esp' -> '0X817'."""
    parts = line.split(b"|")
    return parts[1].decode("ascii", "replace") if len(parts) > 1 else ""


def patch_pi_ini(original_ini: Path, stage: Path) -> dict:
    data = original_ini.read_bytes()
    newline = b"\r\n" if data.count(b"\r\n") else b"\n"
    lines = data.split(newline)

    active = [i for i, line in enumerate(lines)
              if line.lstrip().startswith(b"$SIBADA") and not line.lstrip().startswith(b";")]
    if not active:
        raise GeneratorError(f"no active $SIBADA payload in {original_ini}")

    keep, drop, replace = [], [], []
    for index in active:
        token = payload_form_token(lines[index])
        if token in PI_KEEP_FORM_TOKENS:
            keep.append(index)
        elif token in PI_DROP_FORM_TOKENS:
            drop.append(index)
        else:
            replace.append(index)

    if len(keep) != len(PI_KEEP_FORM_TOKENS):
        raise GeneratorError(f"expected to keep {PI_KEEP_FORM_TOKENS}, matched {len(keep)}")
    if len(drop) != len(PI_DROP_FORM_TOKENS):
        raise GeneratorError(f"expected to drop {PI_DROP_FORM_TOKENS}, matched {len(drop)}")

    patched, emitted_request = [], False
    for index, line in enumerate(lines):
        if index == active[0]:
            patched.append(PI_REQUEST_LINE)
            emitted_request = True
            for kept_index in keep:
                patched.append(lines[kept_index])
            continue
        if index in active:
            continue
        patched.append(line)
    if not emitted_request:
        raise GeneratorError("request cast was never emitted")

    target = stage / PI_INI_REL
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(newline.join(patched))

    # self check: dropping the inserted block must give back the original file without its
    # active payload lines, i.e. no other line was moved, added or rewritten.
    untouched_original = [l for i, l in enumerate(lines) if i not in active]
    inserted = len(keep) + 1
    stripped = patched[:active[0]] + patched[active[0] + inserted:]
    if stripped != untouched_original:
        raise GeneratorError("PI rewrite changed a line it must not touch")

    return {
        "target": str(target),
        "sourceIni": str(original_ini),
        "sourceBytes": len(data),
        "targetBytes": target.stat().st_size,
        "newline": "CRLF" if newline == b"\r\n" else "LF",
        "activePayloadLines": len(active),
        "requestPayload": PI_REQUEST_LINE.decode("ascii"),
        "keptPayloads": [lines[i].decode("ascii", "replace") for i in keep],
        "droppedPayloads": [lines[i].decode("ascii", "replace") for i in drop],
        "replacedPayloads": [lines[i].decode("ascii", "replace") for i in replace],
        "commentedLinesKept": sum(1 for l in lines if l.lstrip().startswith(b";$SIBADA")),
    }


# --------------------------------------------------------------------------- main

def run(mod_root: Path, stage: Path, report_path: Path) -> dict:
    if not mod_root.is_dir():
        raise GeneratorError(f"original mod not found: {mod_root}")

    oar_folder = resolve_below(mod_root, OAR_DIR_PARTS + (REPLACER_MOD,))
    stage_oar = stage / Path(*OAR_DIR_PARTS)

    overlays = [build_override(submod, animations, mod_root, oar_folder, stage_oar)
                for submod, animations in DISABLES.items()]
    ready = [build_ready_submod(source, oar_folder, stage_oar) for source in DISABLES]
    pi = patch_pi_ini(mod_root / PI_INI_REL, stage)

    # Trigger2 and every other submod must stay untouched in the patch tree
    oar_rel = Path(*OAR_DIR_PARTS) / REPLACER_MOD
    for submod in UNTOUCHED_SUBMODS:
        stray = stage / oar_rel / submod
        if stray.exists():
            raise GeneratorError(f"{submod} must not be part of the patch, found {stray}")

    report = {
        "generatedBy": "tools/generate_oar.py",
        "originalMod": str(mod_root),
        "oarModFolder": str(oar_folder),
        "patchPlugin": PATCH_PLUGIN,
        "readyEffect": f"0x{READY_EFFECT_ID:X}",
        "projectNames": list(PROJECT_NAMES),
        "installNote": ("Install this patch mod with a higher MO2 priority than the original mod: "
                        "the three config.json overlays and the ini only take effect when they win "
                        "the file conflict in the virtual Data folder. The HKX files stay in the "
                        "original mod and are reused through overrideAnimationsFolder."),
        "oarFormatEvidence": [
            "build/refs/OAR-2.3.4 git HEAD 6b1fc1e 'Version 2.3.4' (CMakeLists.txt VERSION 2.3.4)",
            "Parsing.h:106 + Parsing.cpp:314-317 read overrideAnimationsFolder from config.json",
            "Parsing.cpp:749-761 resolve it against the submod's parent and load that folder's animations",
            "UI/UIMain.cpp:1141-1162 expose the same field in OAR's own UI",
            "Parsing.cpp:239-308 / ReplacerMods.cpp:70,440-540 define replacementAnimDatas matching",
            "ReplacementAnimation.cpp:279 a disabled replacement is simply not offered",
        ],
        "oarOverlays": overlays,
        "oarNewSubmods": ready,
        "oarUntouchedSubmods": list(UNTOUCHED_SUBMODS),
        "oarStagedFiles": oar_files(stage / oar_rel),
        "payloadInterpreter": pi,
    }

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mod", type=Path, default=DEFAULT_MOD,
                        help="original mod folder (read-only)")
    parser.add_argument("--stage", type=Path, default=DEFAULT_STAGE,
                        help="patch staging folder (written)")
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT,
                        help="json report path (written)")
    args = parser.parse_args(argv)

    try:
        report = run(args.mod, args.stage, args.report)
    except GeneratorError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(f"oar: {len(report['oarOverlays'])} original submod(s) overlaid, "
          f"{len(report['oarNewSubmods'])} new <source>Ready submod(s), "
          f"{len(report['oarStagedFiles'])} file(s) staged")
    for overlay in report["oarOverlays"]:
        print(f"  - {overlay['submod']}: disabled {len(overlay['disabledAnimations'])} "
              f"animation(s) x {len(overlay['projects'])} project(s)")
    for ready in report["oarNewSubmods"]:
        print(f"  - {ready['submod']}: priority {ready['priority']} (from "
              f"{ready['prioritySource']}), keeps {ready['sourceConditionsKept']} original "
              f"condition(s) + 0x{READY_EFFECT_ID:X}, reuses "
              f"'{ready['overrideAnimationsFolder']}'")
    pi = report["payloadInterpreter"]
    print(f"pi: {pi['activePayloadLines']} active $SIBADA payload(s): "
          f"{len(pi['keptPayloads'])} kept, {len(pi['replacedPayloads'])} replaced by the request "
          f"cast, {len(pi['droppedPayloads'])} not castable")
    print(f"    request: {pi['requestPayload']}")
    print(f"report: {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
