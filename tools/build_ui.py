#!/usr/bin/env python3
"""Build the KratosRage HUD widget SWF (AVM1 / ActionScript 2) with a portable MTASC.

  vendor/ui/mtasc.exe                       portable MTASC 1.17 (SWFRecomp fork)
  ui/src/*.as                               sources
  build/ui/meter.swf                        build artifact
  build/ui/meter.tags.txt                   SWF tag/structure audit trail
  build/stage/Interface/exported/widgets/KratosRage/meter.swf   staged for packaging

The SWF must stay AVM1/AS2: Skyrim's Scaleform HUD is AVM1 and SkyUI's widgetloader hands the
loaded root's "widget" child to Papyrus, so an AVM2 (AS3) movie cannot be used at all.

Usage (from anywhere):
    python tools/build_ui.py [--check]

Exit code 0 only when MTASC succeeded, the SWF parses, its header matches the requested stage and
the expected structures (a "widget" child, the bound API names) are present.
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]

MTASC = PROJECT / "vendor" / "ui" / "mtasc.exe"
MTASC_DOWNLOAD = PROJECT / "vendor" / "_downloads" / "mtasc.exe"

SRC_DIR = PROJECT / "ui" / "src"
SOURCES = ["KrMeterBootstrap.as", "KratosRageMeter.as"]

STAGE_WIDTH = 1280
STAGE_HEIGHT = 720
FRAME_RATE = 30
SWF_VERSION = 8  # AVM1, Flash 8 feature set (Scaleform HUD safe)

OUT_DIR = PROJECT / "build" / "ui"
OUT_SWF = OUT_DIR / "meter.swf"
OUT_TAGS = OUT_DIR / "meter.tags.txt"
STAGE_SWF = PROJECT / "build" / "stage" / "Interface" / "exported" / "widgets" / "KratosRage" / "meter.swf"

TAG_NAMES = {
    0: "End", 1: "ShowFrame", 2: "DefineShape", 4: "PlaceObject", 5: "RemoveObject",
    6: "DefineBits", 7: "DefineButton", 9: "SetBackgroundColor", 12: "DoAction",
    20: "DefineBitsLossless", 21: "DefineBitsJPEG2", 22: "DefineShape2", 24: "Protect",
    26: "PlaceObject2", 28: "RemoveObject2", 32: "DefineShape3", 33: "DefineText2",
    34: "DefineButton2", 36: "DefineBitsLossless2", 37: "DefineEditText", 39: "DefineSprite",
    43: "FrameLabel", 45: "SoundStreamHead2", 46: "DefineMorphShape", 48: "DefineFont2",
    56: "ExportAssets", 57: "ImportAssets", 59: "DoInitAction", 60: "DefineVideoStream",
    62: "DefineFontInfo2", 69: "FileAttributes", 70: "PlaceObject3", 71: "ImportAssets2",
    76: "SymbolClass", 77: "Metadata", 82: "DoABC", 86: "DefineSceneAndFrameLabelData",
}

# Names that must appear somewhere in the compiled movie: the child clip the loader looks for, the
# MTASC main entry marker, our classes and the API surface Papyrus invokes.
REQUIRED_STRINGS = [
    b"widget",
    b"MTASC_MAIN",
    b"KrMeterBootstrap",
    b"KratosRageMeter",
    # invoked from KR_Widget.psc (names must match papyrus/KR_Widget.psc exactly)
    b"setPercent",
    b"setReady",
    b"setStyle",
    b"setValue",
    b"showMeter",
    b"hideMeter",
    b"flashDenied",
    b"cancelAnimations",
    # invoked by SKI_WidgetBase / skyui/widgetloader.swf
    b"setHudMetrics",
    b"setRootPath",
    b"setClientInfo",
    b"setModes",
    b"setHAnchor",
    b"setVAnchor",
    b"setPositionX",
    b"setPositionY",
    b"setAlpha",
    b"tweenTo",
    b"fadeTo",
    b"onModeChange",
]


def log(message: str) -> None:
    print(message, flush=True)


def run(cmd: list[str]) -> tuple[int, str]:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return proc.returncode, proc.stdout


def ensure_mtasc() -> bool:
    """MTASC is portable but needs its own AS2 class headers next to the classpath ("std"), plus
    the Flash 8 ones ("std8") for -version 8."""
    if not MTASC.is_file():
        if MTASC_DOWNLOAD.is_file():
            MTASC.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(MTASC_DOWNLOAD, MTASC)
            log(f"  promoted {MTASC_DOWNLOAD} -> {MTASC}")
        else:
            return False
    return (MTASC.parent / "std" / "MovieClip.as").is_file() and (MTASC.parent / "std8" / "MovieClip.as").is_file()


def decompress_swf(path: Path) -> bool:
    """MTASC 1.17 always writes a zlib compressed movie (generate ~compress:true). Skyrim's
    Scaleform can read CWS, but the shipped asset stays an uncompressed FWS like SkyUI's own
    widgets, so the file is rewritten losslessly here."""
    data = path.read_bytes()
    if data[0:3] != b"CWS":
        return False
    body = zlib.decompress(data[8:])
    result = bytearray(b"FWS" + bytes([data[3]]) + b"\x00\x00\x00\x00" + body)
    struct.pack_into("<I", result, 4, len(result))
    path.write_bytes(bytes(result))
    return True


def check_papyrus_contract() -> list[str]:
    """Cross-check the other half of the contract: every SWF method KR_Widget.psc invokes must
    exist in this movie. Uses the disassembly produced by tools/compile_papyrus.py."""
    problems: list[str] = []
    disasm = PROJECT / "build" / "papyrus" / "disasm" / "KR_Widget.txt"
    if not disasm.is_file():
        return problems  # Papyrus build has not run yet; nothing to compare against

    text = disasm.read_text(encoding="utf-8", errors="replace")
    # Every UI.Invoke target is a ".methodName" literal in the PEX string table.
    invoked = sorted(set(re.findall(r"'\.([A-Za-z_][A-Za-z0-9_]*)'", text)))
    shipped = OUT_SWF.read_bytes()
    for name in invoked:
        if name.encode("ascii") not in shipped:
            problems.append(f"KR_Widget.psc invokes .{name} but meter.swf does not define it")
    log(f"  Papyrus invokes {len(invoked)} SWF method(s): {', '.join('.' + n for n in invoked)}")
    return problems


def parse_swf(path: Path) -> dict:
    """Minimal SWF structure reader: header + tag list. Enough to prove what MTASC emitted."""
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError("file too short to be a SWF")

    signature = data[0:3]
    version = data[3]
    file_length = struct.unpack_from("<I", data, 4)[0]
    if signature == b"CWS":
        raise ValueError("compressed SWF; expected the uncompressed FWS written by this build")
    if signature != b"FWS":
        raise ValueError(f"not a SWF: signature {signature!r}")

    # Frame size is a RECT: 5 bits per field, starting at byte 8.
    nbits = data[8] >> 3
    rect_bits = 5 + nbits * 4
    rect_bytes = (rect_bits + 7) // 8
    rect = int.from_bytes(data[8 : 8 + rect_bytes], "big")
    bits = bin(rect)[2:].rjust(rect_bytes * 8, "0")
    values = [int(bits[5 + i * nbits : 5 + (i + 1) * nbits], 2) for i in range(4)]
    stage_w = (values[1] - values[0]) / 20.0
    stage_h = (values[3] - values[2]) / 20.0

    offset = 8 + rect_bytes
    frame_rate = struct.unpack_from("<H", data, offset)[0] / 256.0
    frame_count = struct.unpack_from("<H", data, offset + 2)[0]
    offset += 4

    tags = []
    while offset < len(data):
        code_and_length = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        code = code_and_length >> 6
        length = code_and_length & 0x3F
        if length == 0x3F:
            length = struct.unpack_from("<I", data, offset)[0]
            offset += 4
        tags.append((code, length))
        offset += length
        if code == 0:
            break

    return {
        "signature": signature.decode(),
        "version": version,
        "declared_length": file_length,
        "actual_length": len(data),
        "stage": (stage_w, stage_h),
        "frame_rate": frame_rate,
        "frame_count": frame_count,
        "tags": tags,
        "body": data,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="build and verify only, skip staging")
    args = parser.parse_args()

    log("[1/4] checking the portable MTASC")
    if not ensure_mtasc():
        log(f"ERROR: mtasc.exe / std / std8 not found under {MTASC.parent}")
        log("       mtasc.exe goes to vendor/ui/mtasc.exe; its 'std' and 'std8' class header")
        log("       directories go next to it (vendor/ui/std, vendor/ui/std8).")
        return 2
    code, output = run([str(MTASC)])
    version_line = [line for line in output.splitlines() if "ActionScript2" in line]
    log(f"  {version_line[0].strip() if version_line else output.strip().splitlines()[0]}")

    log("[2/4] compiling ui/src/*.as")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    classpath = ";".join([str(SRC_DIR), str(MTASC.parent)])
    # MTASC 1.17 only generates a SWF when -swf is given (main.ml branches on it); -out then just
    # names the result, and because -header supplies the stage the -swf input is never read.
    args_list = [
        str(MTASC),
        "-version", str(SWF_VERSION),
        "-swf", str(OUT_SWF),
        "-out", str(OUT_SWF),
        "-header", f"{STAGE_WIDTH}:{STAGE_HEIGHT}:{FRAME_RATE}",
        "-main",
        "-cp", classpath,
    ] + [str(SRC_DIR / name) for name in SOURCES]
    code, output = run(args_list)
    log(output.rstrip() if output.strip() else "  (mtasc silent)")
    if code != 0 or not OUT_SWF.is_file():
        log("ERROR: mtasc failed")
        return 2

    log("[3/4] verifying the SWF structure")
    if decompress_swf(OUT_SWF):
        log("  rewrote the MTASC compressed movie as uncompressed FWS")
    try:
        info = parse_swf(OUT_SWF)
    except ValueError as exc:
        log(f"ERROR: {exc}")
        return 3

    problems: list[str] = []
    if info["declared_length"] != info["actual_length"]:
        problems.append(f"header length {info['declared_length']} != file size {info['actual_length']}")
    if abs(info["stage"][0] - STAGE_WIDTH) > 0.5 or abs(info["stage"][1] - STAGE_HEIGHT) > 0.5:
        problems.append(f"stage {info['stage']} != {STAGE_WIDTH}x{STAGE_HEIGHT}")
    if abs(info["frame_rate"] - FRAME_RATE) > 0.01:
        problems.append(f"frame rate {info['frame_rate']} != {FRAME_RATE}")
    if info["frame_count"] < 1:
        problems.append("no frames")
    if not any(code == 0 for code, _ in info["tags"]):
        problems.append("missing End tag")

    codes = [code for code, _ in info["tags"]]
    action_tags = [c for c in codes if c in (12, 59)]
    if not action_tags:
        problems.append("no DoAction/DoInitAction tag: the class code is missing")

    for needle in REQUIRED_STRINGS:
        if needle not in info["body"]:
            problems.append(f"missing name {needle.decode()}")

    problems.extend(check_papyrus_contract())

    lines = [
        "KratosRage meter.swf structure audit",
        f"file       : {OUT_SWF}",
        f"signature  : {info['signature']} version {info['version']}",
        f"length     : {info['actual_length']} bytes",
        f"stage      : {info['stage'][0]:.0f} x {info['stage'][1]:.0f}",
        f"frame rate : {info['frame_rate']}",
        f"frames     : {info['frame_count']}",
        "",
        "tags:",
    ]
    for code, length in info["tags"]:
        lines.append(f"  {code:>3} {TAG_NAMES.get(code, 'Unknown'):<24} {length} bytes")
    OUT_TAGS.write_text("\n".join(lines) + "\n", encoding="utf-8")

    if problems:
        for problem in problems:
            log(f"  ! {problem}")
        log("ERROR: SWF structure verification failed")
        return 3
    log(f"  {OUT_SWF} ({info['actual_length']} B), {len(info['tags'])} tags, stage {info['stage'][0]:.0f}x{info['stage'][1]:.0f} @{info['frame_rate']}")
    log(f"  audit trail: {OUT_TAGS}")

    if args.check:
        log("[4/4] --check: staging skipped")
        return 0

    log("[4/4] staging")
    STAGE_SWF.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(OUT_SWF, STAGE_SWF)
    log(f"  staged {STAGE_SWF} ({STAGE_SWF.stat().st_size} B)")
    log("build/ui OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
