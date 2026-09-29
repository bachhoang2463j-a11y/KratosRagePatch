#!/usr/bin/env python3
"""Read-only audit of a release ZIP against the current stage, sources and original-mod baseline."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import runpy
import unittest
from unittest.mock import patch
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "build/stage"
CONFIG = "SKSE/Plugins/StorageUtilData/KratosRage/config.json"
MANIFEST = "Docs/KratosRage/manifest.json"
DLL = "SKSE/Plugins/KratosRage.dll"
SWF = "Interface/exported/widgets/KratosRage/meter.swf"
# 0.3.0 只换 DLL：除 DLL 与 manifest 外，每个运行资产必须与冻结的 0.2.0 包逐字节一致。
FROZEN_REFERENCE = ROOT / "dist/KratosRagePatch-0.2.0-skyrim-1.6.640.zip"
FROZEN_REFERENCE_SHA256 = "72e4e8dd6a10d57abfc3aafcfaadf6c735f879bc7f001efb3d94c7e2e7eb23df"
RAGE_GAIN_CONTRACT = {
    "mode": "combatHit",
    "event": "TESHitEvent",
    "perHit": 2,
    "perSecondCap": None,
    "deduplication": "none",
    "clearOnCombatEnd": True,
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def swf_component_version(swf):
    """Flash header format byte and the AS2 component version string embedded in the SWF."""
    require(swf[:3] == b"FWS", "Staged SWF is compressed or not a Flash movie; cannot read its version")
    tokens = re.findall(rb"(?<![0-9.])(\d+\.\d+\.\d+)(?![0-9.])", swf)
    require(tokens, "Staged SWF carries no x.y.z component version string")
    return swf[3], tokens[0].decode("ascii")


def check_layout_probes():
    tests = runpy.run_path(str(ROOT / "tests/test_package.py"))
    config = STAGE / CONFIG
    oar = STAGE / tests["OAR_REL"]
    read_bytes = Path.read_bytes
    is_file = Path.is_file
    cases = [
        ("required default configuration accepted", [config], False, True, False),
        ("unknown JSON outside OAR rejected", [STAGE / "SKSE/unexpected.json"], True, True, False),
        ("OAR prefix lookalike rejected", [oar.parent / (oar.name + "-unexpected") / "config.json"], True, True, False),
        ("modified default configuration rejected", [config], True, True, True),
        ("pre-package stage without defaults supported", [], False, False, False),
    ]
    results = []
    for name, paths, should_fail, config_exists, bad_config in cases:
        def read(path):
            return b"{}" if path == config and bad_config else read_bytes(path)

        def exists(path):
            return config_exists if path == config else is_file(path)

        result = unittest.TestResult()
        with patch.object(Path, "rglob", return_value=iter(paths)), \
                patch.object(Path, "read_bytes", read), patch.object(Path, "is_file", exists):
            tests["TestStageLayout"]("test_no_stray_json_outside_the_oar_tree").run(result)
        require(not result.errors and len(result.failures) == int(should_fail),
                f"Layout probe failed: {name}: {result.errors or result.failures}")
        results.append({"case": name, "expectedFailures": int(should_fail),
                        "observedFailures": len(result.failures), "passed": True})
    return results


def audit(archive, reference=FROZEN_REFERENCE):
    checksum = archive.with_suffix(archive.suffix + ".sha256")
    require(checksum.is_file(), f"Release SHA-256 file is missing: {checksum}")
    archive_hash = sha256(archive.read_bytes())
    require(checksum.read_text(encoding="utf-8").rstrip("\r\n") == f"{archive_hash}  {archive.name}",
            "Release SHA-256 file does not match the archive")
    baseline = json.loads((ROOT / "build/resume-original-sha256.json").read_text(encoding="utf-8"))
    original = Path(baseline["root"])
    require(original.is_dir() and baseline["files"], "Original mod or baseline is missing")
    current = {p.relative_to(original).as_posix(): sha256(p.read_bytes())
               for p in original.rglob("*") if p.is_file()}
    require(current == baseline["files"], "Original mod differs from the saved baseline")

    require(reference.is_file(), f"Frozen 0.2.0 reference archive is missing: {reference}")
    require(sha256(reference.read_bytes()) == FROZEN_REFERENCE_SHA256,
            "Frozen 0.2.0 reference archive SHA-256 changed")
    expected = {p.relative_to(STAGE).as_posix(): p.read_bytes()
                for p in STAGE.rglob("*") if p.is_file()}
    runtime_count = len(expected)
    require(runtime_count == 16, f"Expected 16 runtime files, got {runtime_count}")
    require(expected.get(CONFIG) == (ROOT / "data/config.default.json").read_bytes(),
            "Default configuration is missing or changed")
    require(expected.get(DLL) ==
            (ROOT / "build/native/Release/KratosRage.dll").read_bytes(), "Staged DLL differs from Release")
    for dependency in ("CommonLibSSE-NG", "fmt", "spdlog", "rapidcsv"):
        expected[f"Docs/KratosRage/licenses/{dependency}.txt"] = (ROOT / "vendor" / dependency / "LICENSE").read_bytes()

    report = json.loads((ROOT / "build/PluginBuilder/build-report.json").read_text(encoding="utf-8"))["verify"]
    require(report["passed"] and report["failed"] == 0 and
            report["total"] == len(report["checks"]) and all(c["passed"] for c in report["checks"]),
            "Plugin verification did not pass")

    with ZipFile(archive) as zipped:
        names = zipped.namelist()
        require(zipped.testzip() is None, "ZIP CRC check failed")
        require(len(names) == len(set(names)) == len({n.casefold() for n in names}),
                "Duplicate ZIP paths or case collisions")
        require(all(not PurePosixPath(n).is_absolute() and ".." not in PurePosixPath(n).parts
                    and ":" not in n and "\\" not in n and not n.endswith("/") for n in names),
                "Unsafe or unexpected ZIP path")
        require(set(names) == set(expected) | {MANIFEST}, "ZIP file set differs from the current stage and docs")
        require([n for n in names if n.lower().endswith(".dll")] == [DLL],
                "Unexpected DLL in the release")
        require(not any(n.lower().endswith((".hkx", ".exe", ".pdb", ".lib", ".bsa")) for n in names),
                "Unwanted animation, tool or build artifact in ZIP")
        manifest = json.loads(zipped.read(MANIFEST))
        require(manifest["version"] == "0.3.0" and manifest["runtime"] == "1.6.640"
                and manifest["gameRuntimeTested"] is False, "Wrong release version or runtime claim")
        require(manifest.get("diagnostic") is False and manifest.get("hudAlwaysVisible") is True
                and manifest.get("rageGain") == RAGE_GAIN_CONTRACT,
                "Wrong combat hit contract")
        require(manifest.get("hudDisplay") == {"numericReadout": False, "stepPercent": 10,
                                               "nonFullMaximumPercent": 90},
                "Wrong HUD display contract")
        require(manifest["originalFilesUnchanged"] == len(current)
                and manifest["pluginChecksPassed"] == report["total"], "Manifest verification totals differ")
        require(set(manifest["files"]) == set(expected), "Manifest file set differs")
        for name, content in expected.items():
            data = zipped.read(name)
            require(data == content, f"ZIP differs from current source or stage: {name}")
            require(manifest["files"][name] == {"size": len(data), "sha256": sha256(data)},
                    f"Manifest size or SHA-256 mismatch: {name}")

        # 0.3.0 只换 DLL：冻结的 0.2.0 包必须存在，且除 DLL 与 manifest 外逐字节一致。
        with ZipFile(reference) as frozen:
            require(frozen.testzip() is None, "Frozen 0.2.0 reference ZIP CRC check failed")
            require(set(frozen.namelist()) == set(names),
                    "Release file set differs from the frozen 0.2.0 archive")
            require(zipped.read(DLL) != frozen.read(DLL), "Release still contains the frozen 0.2.0 DLL")
            reused = [n for n in names if n not in (DLL, MANIFEST)]
            for name in reused:
                require(zipped.read(name) == frozen.read(name),
                        f"Reused 0.2.0 asset changed byte-for-byte: {name}")
        swf_format, swf_version = swf_component_version(expected[SWF])
        require((swf_format, swf_version) == (8, "0.2.0"), "Wrong reused SWF format or component version")

    return {
        "archive": str(archive.resolve()),
        "size": archive.stat().st_size,
        "sha256": archive_hash,
        "checksumFileVerified": str(checksum.resolve()),
        "files": len(names),
        "runtimeFiles": runtime_count,
        "manifestEntriesVerified": len(expected),
        "originalFilesUnchanged": len(current),
        "pluginChecksPassed": report["total"],
        "frozenReference": str(reference.resolve()),
        "frozenReferenceSha256": sha256(reference.read_bytes()),
        "reusedAssetsFrom0_2_0": len(reused),
        "reusedAssetsByteIdentical": True,
        "swfFormatVersion": swf_format,
        "swfComponentVersion": swf_version,
        "crcValid": True,
        "pathSafetyAndCaseUniqueness": True,
        "stageAndDocsBytesMatch": True,
        "layoutProbes": check_layout_probes(),
        "gameRuntimeTested": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path,
                        default=ROOT / "dist/KratosRagePatch-0.3.0-skyrim-1.6.640.zip")
    parser.add_argument("--reference", type=Path, default=FROZEN_REFERENCE,
                        help="Alternate path to the same frozen 0.2.0 archive; its SHA-256 must match")
    args = parser.parse_args()
    print(json.dumps(audit(args.archive, args.reference), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
