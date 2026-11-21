"""Export an allowlisted, audited source distribution; omit generated/private files."""
import argparse
import hashlib
import json
import re
import shutil
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = {'README.md', '使用说明.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'AI_DISCLOSURE.md', '.gitignore', '.gitattributes', 'CMakeLists.txt', 'build.ps1', 'pilot.ps1', 'start-model.ps1', 'run-agent.ps1', 'Start.cmd'}
DIRECTORIES = {'src', 'include', 'cmake', 'scripts', 'tests', 'fixtures', 'docs', 'examples', 'third_party', '.github', 'assets', 'launcher'}
DENIED_PARTS = {'.git', '.agent_data', '.runtime', 'models', '__pycache__', '.vs', '.vscode', 'dist', 'release-check', 'tmp', 'temp'}
ASSET_FILES = {'assets/README.md', 'assets/app.ico', 'assets/icon-pixel.png'}
DENIED_EXTENSIONS = {'.exe', '.dll', '.obj', '.o', '.a', '.lib', '.pdb', '.ilk', '.log', '.pyc', '.pyo', '.gguf', '.safetensors', '.pt', '.pth', '.zip', '.pem', '.key', '.pfx'}
DENIED_EXTENSIONS.update({'.tmp', '.bak', '.orig'})
SECRETS = re.compile(r'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|sk-[A-Za-z0-9_-]{30,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)')
PRIVATE_PATHS = re.compile(r'(?:\b[A-Za-z]:[\\/]+[^\s"\r\n]+|/(?:home|Users)/[^\s"\r\n]+)', re.I)

def export_files():
    files = []
    for name in sorted(FILES):
        file = ROOT / name
        if not file.is_file(): raise RuntimeError('Missing release file: ' + name)
        files.append(file)
    for name in sorted(DIRECTORIES):
        folder = ROOT / name
        if not folder.is_dir(): raise RuntimeError('Missing release directory: ' + name)
        for file in sorted(folder.rglob('*')):
            relative = file.relative_to(ROOT)
            if name == 'assets' and relative.as_posix() not in ASSET_FILES: continue
            if file.is_symlink(): raise RuntimeError('Symlink in source export: ' + relative.as_posix())
            if any(part in DENIED_PARTS or part.startswith('build') for part in relative.parts[1:-1]): continue
            if not file.is_file() or file.suffix.lower() in DENIED_EXTENSIONS or file.name.startswith('.env') or file.name.endswith('~'): continue
            files.append(file)
    for file in files:
        relative = file.relative_to(ROOT)
        if any(part in DENIED_PARTS for part in relative.parts): raise RuntimeError('Private path selected: ' + relative.as_posix())
        if file.stat().st_size > 4 * 1024 * 1024: raise RuntimeError('Unexpected large source file: ' + relative.as_posix())
        image_signatures = {'.png': b'\x89PNG\r\n\x1a\n', '.ico': b'\x00\x00\x01\x00'}
        if file.suffix.lower() in image_signatures:
            if not file.read_bytes().startswith(image_signatures[file.suffix.lower()]):
                raise RuntimeError('Invalid icon format: ' + relative.as_posix())
            continue
        text = file.read_text(encoding='utf-8-sig')
        if SECRETS.search(text): raise RuntimeError('Potential credential in ' + relative.as_posix())
        if PRIVATE_PATHS.search(text): raise RuntimeError('Private machine path in ' + relative.as_posix())
    return files

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Audit without packaging')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    options = parser.parse_args()
    files = export_files()
    print(f'Audit passed: {len(files)} source files; no generated state, models, known credentials or private machine paths.')
    if options.check: return
    version = re.search(r'project\(CppPilot VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text()).group(1)
    name = 'CppPilot-v' + version
    output = options.output.resolve()
    destination = output / name
    if destination.exists(): raise RuntimeError('Destination exists; choose a fresh --output. Existing files are preserved: ' + str(destination))
    destination.mkdir(parents=True)
    manifest = {'version': version, 'created_utc': datetime.now(timezone.utc).isoformat(), 'files': []}
    for file in files:
        relative = file.relative_to(ROOT)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, target)
        manifest['files'].append({'path': relative.as_posix(), 'bytes': file.stat().st_size, 'sha256': hashlib.sha256(file.read_bytes()).hexdigest()})
    (destination / 'SOURCE_MANIFEST.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    archive = output / (name + '-source.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as package:
        for entry in manifest['files']:
            package.write(destination / entry['path'], (Path(name) / entry['path']).as_posix())
        package.write(destination / 'SOURCE_MANIFEST.json', name + '/SOURCE_MANIFEST.json')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / (archive.name + '.sha256')).write_text(digest + '  ' + archive.name + '\n', encoding='ascii')
    print('Source directory:', destination)
    print('Source archive:', archive)
    print('SHA-256:', digest)

if __name__ == '__main__':
    try: main()
    except (OSError, RuntimeError, UnicodeError) as error: sys.exit('Source packaging failed: ' + str(error))
