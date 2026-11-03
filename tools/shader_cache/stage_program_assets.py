"""Shader-prep assets for the program package: strict input contract.

Opt-in inputs are a shader_seed.py build output (seed manifest + every listed
cache entry + portable PSO inventory) and MacrossShaderPrep.exe. Both or
neither; partial, mismatched or tampered inputs are rejected, as are local
GPU/driver artifacts (pso-library*, pso-observed*, coverage-misses*). The game
executable must carry the shader-prep hooks, otherwise shipping the assets
would be meaningless and is refused.

The launcher stages every bin/ row of program-manifest.json
(tools/launcher/PrebuiltInstall.cs), and launcher-python/graphics_prep.py
expects exactly bin/MacrossShaderPrep.exe + bin/assets/... next to the game.

    python tools/shader_cache/stage_program_assets.py --seed <build output>
           --prep-tool <MacrossShaderPrep.exe> --exe <Macross30Recompiled.exe> --program <staging> [--dry-run]
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import shader_seed  # noqa: E402

TOOL_PATH = 'bin/MacrossShaderPrep.exe'
MANIFEST_PATH = 'bin/assets/' + shader_seed.MANIFEST
INVENTORY_PATH = 'bin/' + shader_seed.INVENTORY
ENTRY_RE = re.compile(r'^bin/assets/rsx_shader_cache_v2/[VPB]_[0-9A-F]{16}_[0-9A-F]{8}\.bin$')
EXE_HOOK_MARKERS = (b'[shader-prep] init', b'[pso-library-hit]')
TOOL_MARKERS = (b'MacrossShaderPrep', b'pso-library-v1.bin')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def is_local_only(name):
    return PurePosixPath(name).name.startswith(shader_seed.LOCAL_ONLY_PREFIXES)


def program_files(seed, tool, exe):
    """{program-relative path: source path} plus a summary; raises ValueError."""
    if (seed is None) != (tool is None):
        raise ValueError('shader-prep inputs are all-or-nothing: give both the seed build and MacrossShaderPrep.exe')
    if seed is None:
        return {}, None
    seed, tool, exe = Path(seed), Path(tool), Path(exe)
    result = shader_seed.verify_seed(seed)
    if not result['ok']:
        raise ValueError('shader seed failed verification: ' + json.dumps(result['errors'])[:2000])
    manifest = json.loads((seed / shader_seed.MANIFEST).read_text(encoding='utf-8'))
    if not manifest['pso_inventory']['path'] or not manifest['pso_inventory']['records']:
        raise ValueError('shader seed has no portable PSO inventory')
    data = tool.read_bytes() if tool.is_file() else b''
    if data[:2] != b'MZ' or not all(m in data for m in TOOL_MARKERS):
        raise ValueError('not a MacrossShaderPrep executable: ' + str(tool))
    game = exe.read_bytes()
    if not all(m in game for m in EXE_HOOK_MARKERS):
        raise ValueError('game executable lacks the shader-prep hooks; refusing to ship seed assets')
    files = {TOOL_PATH: tool, MANIFEST_PATH: seed / shader_seed.MANIFEST,
             INVENTORY_PATH: seed / shader_seed.INVENTORY}
    for entry in manifest['entries']:
        files[f"bin/{shader_seed.SEED_DIR}/{entry['name']}"] = seed / shader_seed.SEED_DIR / entry['name']
    summary = dict(manifest_sha256=manifest['manifest_sha256'], entries=manifest['entry_count'],
                   stages=manifest['stages'], seed_bytes=manifest['seed_bytes'],
                   inventory_sha256=manifest['pso_inventory']['sha256'], inventory_records=manifest['pso_inventory']['records'],
                   prep_tool_sha256=sha(tool), compiler=manifest['compiler'],
                   local_library_shipped=False)
    return files, summary


def check_package_rows(names, read):
    """Audit side: names are program-relative paths in a package; read(path) -> bytes.
    Returns a summary or None (no shader-prep assets); raises ValueError."""
    for name in names:
        if is_local_only(name):
            raise ValueError('local GPU/driver artifact in package: ' + name)
    entries = sorted(n for n in names if n.startswith('bin/assets/rsx_shader_cache_v2/'))
    present = [n for n in (TOOL_PATH, MANIFEST_PATH, INVENTORY_PATH) if n in names]
    if not entries and not present:
        return None
    if len(present) != 3:
        raise ValueError('partial shader-prep assets: ' + ', '.join(present))
    manifest = json.loads(read(MANIFEST_PATH))
    claimed = manifest.pop('manifest_sha256')
    if shader_seed.sha256(json.dumps(manifest, indent=2, sort_keys=True).encode()) != claimed:
        raise ValueError('shader seed manifest digest mismatch')
    listed = {f"bin/{shader_seed.SEED_DIR}/{m['name']}": m for m in manifest['entries']}
    if set(entries) != set(listed):
        raise ValueError('shader seed entries differ from manifest')
    for name in entries:
        if not ENTRY_RE.match(name):
            raise ValueError('unexpected shader seed member: ' + name)
        data = read(name)
        if shader_seed.sha256(data) != listed[name]['sha256']:
            raise ValueError('shader seed entry hash mismatch: ' + name)
        shader_seed.parse_entry(PurePosixPath(name).name, data)  # YZSC header, source hash, DXBC checksum
    inventory = read(INVENTORY_PATH)
    if shader_seed.sha256(inventory) != manifest['pso_inventory']['sha256'] or inventory[:4] != b'RSPI':
        raise ValueError('PSO inventory does not match the seed manifest')
    tool = read(TOOL_PATH)
    if tool[:2] != b'MZ' or not all(m in tool for m in TOOL_MARKERS):
        raise ValueError('MacrossShaderPrep.exe is not the preparation tool')
    return dict(entries=len(entries), manifest_sha256=claimed, inventory_sha256=manifest['pso_inventory']['sha256'],
                prep_tool_sha256=shader_seed.sha256(tool))


def is_allowed_bin(name):
    """Audit allow-list for '.bin' members (validated by check_package_rows)."""
    return name == INVENTORY_PATH or bool(ENTRY_RE.match(name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed', type=Path, required=True)
    ap.add_argument('--prep-tool', type=Path, required=True)
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--program', type=Path, required=True)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    files, summary = program_files(a.seed, a.prep_tool, a.exe)
    stale_dir = a.program / 'bin' / shader_seed.SEED_DIR
    keep = {(a.program / rel).resolve() for rel in files}
    stale = [p for p in stale_dir.glob('*.bin') if p.resolve() not in keep] if stale_dir.exists() else []
    rows = []
    for rel, source in sorted(files.items()):
        if not a.dry_run:
            target = a.program / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        rows.append(dict(path=rel, sha256=sha(source), bytes=source.stat().st_size))
    if not a.dry_run:
        for p in stale:
            p.unlink()
    print(json.dumps(dict(summary=summary, rows=rows, removed_stale=[str(p) for p in stale], dry_run=a.dry_run), indent=2))


if __name__ == '__main__':
    main()
