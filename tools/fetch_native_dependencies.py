from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
from pathlib import Path
from urllib.request import Request, urlopen
from zipfile import ZipFile
import io
import json

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'vendor'
SOURCES = {
    'cmake': 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip',
    'CommonLibSSE-NG': 'https://codeload.github.com/CharmedBaryon/CommonLibSSE-NG/zip/refs/tags/v3.7.0',
    'spdlog': 'https://codeload.github.com/gabime/spdlog/zip/refs/tags/v1.15.3',
    'rapidcsv': 'https://codeload.github.com/d99kris/rapidcsv/zip/refs/tags/v8.84',
    'fmt': 'https://codeload.github.com/fmtlib/fmt/zip/refs/tags/11.2.0',
}

EXISTING = {}


def load_existing():
    manifest = VENDOR / 'native-dependencies.json'
    if not manifest.exists():
        return {}
    try:
        recorded = json.loads(manifest.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return {entry['name']: entry for entry in recorded if isinstance(entry, dict) and entry.get('name')}


def fetch(item):
    name, url = item
    target = VENDOR / name
    if target.exists() and any(target.iterdir()):
        record = {'name': name, 'url': url, 'existing': True}
        previous = EXISTING.get(name, {})
        if 'sha256' in previous:
            record['sha256'] = previous['sha256']
        if 'size' in previous:
            record['size'] = previous['size']
        return record
    raw = urlopen(Request(url, headers={'User-Agent': 'KratosRagePatch-build'}), timeout=120).read()
    with ZipFile(io.BytesIO(raw)) as archive:
        for member in archive.infolist():
            parts = Path(member.filename).parts[1:]
            if not parts or member.is_dir():
                continue
            destination = target.joinpath(*parts)
            if not destination.resolve().is_relative_to(target.resolve()):
                raise ValueError('Archive path escaped dependency directory')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.read(member))
    return {'name': name, 'url': url, 'sha256': sha256(raw).hexdigest(), 'size': len(raw)}


if __name__ == '__main__':
    EXISTING.update(load_existing())
    result = list(ThreadPoolExecutor(max_workers=4).map(fetch, SOURCES.items()))
    (VENDOR / 'native-dependencies.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
