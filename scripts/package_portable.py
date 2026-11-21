"""Package an already-built portable Windows executable and console launcher."""
import argparse
import hashlib
import json
import os
import re
import struct
import zipfile
from pathlib import Path

from package_source import ROOT, export_files


def imports(data):
    """Read PE imports so a package cannot silently omit non-system DLLs."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0':
        raise RuntimeError('Not a Windows PE executable')
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    if machine != 0x8664:
        raise RuntimeError('Expected a Windows x64 executable')
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from('<H', data, optional)[0] != 0x20b:
        raise RuntimeError('Expected PE32+ optional header')
    section_start = optional + optional_size
    def offset(rva):
        for index in range(sections):
            position = section_start + 40 * index
            virtual_size, virtual, raw_size, raw = struct.unpack_from('<IIII', data, position + 8)
            if virtual <= rva < virtual + max(virtual_size, raw_size):
                return raw + rva - virtual
        raise RuntimeError('Invalid import address')
    import_rva = struct.unpack_from('<I', data, optional + 120)[0]
    if not import_rva:
        return []
    position = offset(import_rva)
    names = []
    while any(data[position:position + 20]):
        name = offset(struct.unpack_from('<I', data, position + 12)[0])
        names.append(data[name:data.index(b'\0', name)].decode('ascii').lower())
        position += 20
    return sorted(names)


def package(build, output):
    export_files()  # Reuse the source privacy and credential audit.
    binary = build / 'bin/cpp_agent.exe'
    config = (build / 'generated/pilot/build_config.hpp').read_text(encoding='utf-8-sig')
    if 'cmake = R"(cmake)"' not in config or 'compiler = R"()"' not in config:
        raise RuntimeError('Configure with -DCPPPILOT_PORTABLE=ON before packaging')
    data = binary.read_bytes()
    for value in (str(ROOT), str(build), os.environ.get('USERPROFILE', '')):
        if not value:
            continue
        for variant in (value, value.replace('\\', '/'), value.replace('/', '\\')):
            for encoding in ('utf-8', 'utf-16-le'):
                if variant.lower().encode(encoding) in data.lower():
                    raise RuntimeError('Executable contains a private build or profile path')
    dependencies = imports(data)
    system = {'kernel32.dll', 'msvcrt.dll', 'ucrtbase.dll', 'user32.dll', 'advapi32.dll',
              'shell32.dll', 'winhttp.dll', 'bcrypt.dll', 'ole32.dll', 'oleaut32.dll',
              'ws2_32.dll', 'gdi32.dll', 'comdlg32.dll', 'shlwapi.dll', 'ntdll.dll',
              'crypt32.dll', 'secur32.dll', 'version.dll', 'comctl32.dll'}
    missing = [name for name in dependencies if name not in system and not name.startswith(('api-ms-win-', 'ext-ms-win-'))]
    if missing:
        raise RuntimeError('Non-system DLLs must be statically linked: ' + ', '.join(missing))
    selected = ['Start.cmd', 'launcher/Start.ps1', 'launcher/guide.html', 'start-model.ps1',
                'LICENSE', 'THIRD_PARTY_NOTICES.md', 'AI_DISCLOSURE.md', 'README.md', '使用说明.md',
                'third_party/nlohmann/LICENSE.MIT',
                'assets/README.md', 'assets/icon-pixel.png']
    selected += [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / 'fixtures/repair_project').iterdir()) if p.is_file()]
    selected += [p.relative_to(ROOT).as_posix() for folder in ('docs', 'examples')
                 for p in sorted((ROOT / folder).rglob('*.md'))]
    selected += [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / 'third_party/runtime').glob('*.txt'))]
    version = re.search(r'project\(CppPilot VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text()).group(1)
    name = 'CppPilot-v' + version + '-windows-x64'
    output.mkdir(parents=True, exist_ok=True)
    archive = output / (name + '.zip')
    if archive.exists():
        raise RuntimeError('Archive already exists; choose a fresh --output')
    entries = {rel: (ROOT / rel).read_bytes() for rel in selected}
    entries['bin/cpp_agent.exe'] = data
    manifest = {'version': version, 'platform': 'windows-x64', 'system_dlls': dependencies,
                'files': [{'path': path, 'sha256': hashlib.sha256(content).hexdigest()}
                          for path, content in sorted(entries.items())]}
    entries['PORTABLE_MANIFEST.json'] = json.dumps(manifest, ensure_ascii=False, indent=2).encode('utf-8')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as target:
        for path, content in entries.items():
            target.writestr(name + '/' + path, content)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(digest + '  ' + archive.name + '\n', encoding='ascii')
    print('Portable archive:', archive.name)
    print('Files:', len(entries), '| System DLL imports:', ', '.join(dependencies))
    return archive


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build_portable')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/portable')
    args = parser.parse_args()
    package(args.build_dir.resolve(), args.output.resolve())
