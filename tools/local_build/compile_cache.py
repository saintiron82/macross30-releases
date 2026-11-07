"""Skip compilation only when every compiler input and the final EXE still match."""
import argparse
import hashlib
import json
from pathlib import Path

from incremental import digest, write_changed

SUFFIXES = {'.c', '.cpp', '.h', '.hpp', '.inc', '.cmake', '.py', '.lib'}


def inputs(root):
    rows = []
    for directory in ('native/macross30', 'vendor/ps3recomp-master', 'vendor/SDL2'):
        for path in sorted((root / directory).rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts or 'tests' in path.parts:
                continue
            if path.suffix in SUFFIXES or path.name == 'CMakeLists.txt':
                rows.append((path.relative_to(root).as_posix(), digest(path)))
    cache = root / 'native/build/CMakeCache.txt'
    if cache.exists():
        selected = [line for line in cache.read_text(encoding='utf8').splitlines() if line.startswith(
            ('MACROSS_', 'PORT_', 'CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_BUILD_TYPE'))]
        rows.append(('build-configuration', hashlib.sha256('\n'.join(selected).encode()).hexdigest()))
    # The directly included guard header and caption table are covered above;
    # language resource patches and movie subtitles deliberately are not.
    return hashlib.sha256(json.dumps(rows, separators=(',', ':')).encode()).hexdigest()


def tool_identity(tools):
    rows = []
    for name in tools:
        path = Path(name).resolve()
        stat = path.stat()
        rows.append((str(path), stat.st_size, stat.st_mtime_ns))
    return hashlib.sha256(json.dumps([rows, 'release-O1-ffp-contract-off-posttask-on-v1']).encode()).hexdigest()


def check(root, tools, configuration=""):
    state_path = root / 'native/build/local-compile-cache.json'
    exe = root / 'native/build/Macross30Recompiled.exe'
    current = dict(inputs=inputs(root), tools=tool_identity(tools), configuration=configuration)
    if state_path.exists():
        old = json.loads(state_path.read_text())
        if old.get('tools') != current['tools']:
            return 11, current
        if old.get('configuration', '') == configuration and exe.exists() and old.get('inputs') == current['inputs'] and old.get('exe_sha256') == digest(exe):
            return 0, current
    return 10, current


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--tool', action='append', required=True)
    parser.add_argument('--configuration', default='')
    parser.add_argument('--commit', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    code, current = check(root, args.tool, args.configuration)
    if args.commit:
        current['exe_sha256'] = digest(root / 'native/build/Macross30Recompiled.exe')
        write_changed(root / 'native/build/local-compile-cache.json', (json.dumps(current, indent=2) + '\n').encode())
        return
    print('LOCAL_COMPILE_CACHE: ' + ('unchanged' if code == 0 else 'toolchain changed' if code == 11 else 'changed inputs'), flush=True)
    raise SystemExit(code)


if __name__ == '__main__':
    main()
