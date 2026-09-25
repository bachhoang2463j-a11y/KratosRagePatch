import argparse
import hashlib
import json
from pathlib import Path
import struct
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'build/stage'
CONFIG = 'SKSE/Plugins/StorageUtilData/KratosRage/config.json'
OAR = 'meshes/actors/character/animations/OpenAnimationReplacer/KratosMoveset'
REQUIRED = {
    'KratosRagePatch.esp',
    'Seq/KratosRagePatch.seq',
    'SKSE/Plugins/KratosRage.dll',
    'SKSE/PayloadInterpreter/Config/God of War4.ini',
    'Interface/exported/widgets/KratosRage/meter.swf',
    CONFIG,
    *(f'Scripts/{name}.pex' for name in ('KR_Config', 'KR_Native', 'KR_PlayerAlias', 'KR_Widget')),
    *(f'{OAR}/{group}{suffix}/config.json'
      for group in ('SpartanRageTrigger', 'Barehanded', 'LeviathanAxe')
      for suffix in ('', 'Ready')),
}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def original_unchanged(baseline):
    saved = json.loads(baseline.read_text(encoding='utf-8'))
    original = Path(saved['root'])
    current = {
        path.relative_to(original).as_posix(): digest(path)
        for path in original.rglob('*') if path.is_file()
    }
    if current != saved['files']:
        changed = sorted(name for name in current.keys() | saved['files'].keys()
                         if current.get(name) != saved['files'].get(name))
        raise ValueError(f'Original mod differs from the session baseline: {changed}')
    return len(current)


def validate_stage():
    config_path = STAGE / CONFIG
    default = (ROOT / 'data/config.default.json').read_bytes()
    if config_path.exists() and config_path.read_bytes() != default:
        raise ValueError('Stage config differs from defaults; refusing to overwrite it')
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_bytes(default)
    actual = {path.relative_to(STAGE).as_posix() for path in STAGE.rglob('*') if path.is_file()}
    if actual != REQUIRED:
        raise ValueError(f'Stage mismatch: missing={sorted(REQUIRED - actual)}, '
                         f'unexpected={sorted(actual - REQUIRED)}')
    for name in REQUIRED:
        if (STAGE / name).stat().st_size == 0:
            raise ValueError(f'Empty release file: {name}')
    plugin = (STAGE / 'KratosRagePatch.esp').read_bytes()
    if plugin[:4] != b'TES4' or not struct.unpack_from('<I', plugin, 8)[0] & 0x200:
        raise ValueError('Patch is not an ESL-flagged ESP')
    dll = (STAGE / 'SKSE/Plugins/KratosRage.dll').read_bytes()
    pe_offset = struct.unpack_from('<I', dll, 0x3C)[0]
    if dll[:2] != b'MZ' or dll[pe_offset:pe_offset + 4] != b'PE\0\0':
        raise ValueError('Native DLL is not a PE binary')
    if struct.unpack_from('<H', dll, pe_offset + 4)[0] != 0x8664:
        raise ValueError('Native DLL is not x64')
    for symbol in (b'SKSEPlugin_Load\0', b'SKSEPlugin_Version\0'):
        if symbol not in dll:
            raise ValueError(f'Missing SKSE entry point: {symbol!r}')
    for name in ('KR_Config', 'KR_Native', 'KR_PlayerAlias', 'KR_Widget'):
        if (STAGE / f'Scripts/{name}.pex').read_bytes()[:4] != b'\xFA\x57\xC0\xDE':
            raise ValueError(f'Invalid PEX: {name}')
    if (STAGE / 'Interface/exported/widgets/KratosRage/meter.swf').read_bytes()[:3] not in (b'FWS', b'CWS'):
        raise ValueError('Invalid SWF')
    report = json.loads((ROOT / 'build/PluginBuilder/build-report.json').read_text(encoding='utf-8'))
    if not report['verify']['passed'] or report['verify']['failed']:
        raise ValueError('PluginBuilder verification did not pass')
    return report['verify']['total']


def main():
    parser = argparse.ArgumentParser(description='Package the verified independent MO2 patch')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'dist/KratosRagePatch-0.1.1-ui-r1-skyrim-1.6.640.zip')
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/resume-original-sha256.json')
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(f'Release already exists; choose a new output name: {args.output}')
    original_count = original_unchanged(args.baseline)
    plugin_checks = validate_stage()
    files = {name: (STAGE / name).read_bytes() for name in sorted(REQUIRED)}
    for dependency in ('CommonLibSSE-NG', 'fmt', 'spdlog', 'rapidcsv'):
        files[f'Docs/KratosRage/licenses/{dependency}.txt'] = (ROOT / 'vendor' / dependency / 'LICENSE').read_bytes()
    manifest = {
        'version': '0.1.1',
        'uiRevision': 'r1',
        'hudDisplay': {'numericReadout': False, 'stepPercent': 10, 'nonFullMaximumPercent': 90},
        'runtime': '1.6.640',
        'diagnostic': True,
        'rageGain': {'perPlayerDamage': 2, 'rollingSecondMaximum': 10},
        'hudAlwaysVisible': True,
        'gameRuntimeTested': False,
        'originalFilesUnchanged': original_count,
        'pluginChecksPassed': plugin_checks,
        'files': {name: {'size': len(content), 'sha256': hashlib.sha256(content).hexdigest()}
                  for name, content in sorted(files.items())},
    }
    files['Docs/KratosRage/manifest.json'] = json.dumps(manifest, ensure_ascii=False, indent=2).encode('utf-8')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, 'x', compression=ZIP_DEFLATED, compresslevel=9) as archive:
        for name, content in sorted(files.items()):
            info = ZipInfo(name, date_time=(2026, 9, 25, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, content)
    with ZipFile(args.output) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(files):
            raise ValueError('ZIP validation failed')
        for name, content in files.items():
            if archive.read(name) != content:
                raise ValueError(f'ZIP content mismatch: {name}')
    print(json.dumps({'output': str(args.output), 'size': args.output.stat().st_size,
                      'sha256': digest(args.output), 'files': len(files),
                      'originalFilesUnchanged': original_count}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
