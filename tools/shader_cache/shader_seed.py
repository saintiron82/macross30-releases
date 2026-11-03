"""Shader seed and PSO inventory: scan, deterministic build, verify, coverage.

Inputs are cache-format files written by the renderer (V_/P_/B_*.bin in a
user cache or an earlier seed) and PSO descriptor files (pso-observed-v1.bin /
rsx_pso_inventory_v1.bin). Nothing here guesses shader variants: every seeded
entry is a complete generated (or built-in) source that a real run produced,
recompiled at build time with d3dcompiler_47 and checked against the DXBC the
runtime produced. Driver pipeline blobs are never part of the output.

    python tools/shader_cache/shader_seed.py scan <dir>...
    python tools/shader_cache/shader_seed.py build --from <dir>... [--observed <file>...]
            [--inventory-in <file>...] --out <new dir> [--source-root <tree>]
    python tools/shader_cache/shader_seed.py verify <out dir> [--recompile]
    python tools/shader_cache/shader_seed.py coverage <stderr.log> [--seed <out dir>] [--json <file>]
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import d3dc  # noqa: E402

CACHE_MAGIC = 0x43535A59
CACHE_VERSION = 2
HEADER = struct.Struct('<6IQ')
NAME_RE = re.compile(r'^([VPB])_([0-9A-F]{16})_([0-9A-F]{8})\.bin$')
BUILTIN_TAG = b'RSPB1\n'
FNV_SEED = 1469598103934665603
FNV_SEED_HI = 0x84222325CBF29CE4
RECORD_WORDS = 242
RECORD_BYTES = RECORD_WORDS * 4
FILE_MAGIC, FILE_VERSION = 0x49505352, 1
CHUNK_ROOT, CHUNK_PSOD = 0x544F4F52, 0x444F5350
SEED_DIR = 'assets/rsx_shader_cache_v2'
INVENTORY = 'assets/rsx_pso_inventory_v1.bin'
MANIFEST = 'shader-seed-manifest-v3.json'
# Files the renderer/prep tool write on the user's PC; never part of a seed or package.
LOCAL_ONLY_PREFIXES = ('pso-library', 'pso-observed', 'coverage-misses', 'prep.cancel')
GENERATOR_FILES = ['vendor/ps3recomp-master/libs/video/rsx_live_draw.c',
                   'vendor/ps3recomp-master/libs/video/rsx_fp_decompiler.c',
                   'vendor/ps3recomp-master/libs/video/rsx_vp_decompiler.c',
                   'vendor/ps3recomp-master/libs/video/rsx_macross_bloom_shader.h']


def fnv1a(data: bytes, h: int = FNV_SEED) -> int:
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# Cache entries
# ---------------------------------------------------------------------------
class EntryError(ValueError):
    pass


def parse_entry(name: str, data: bytes) -> dict:
    m = NAME_RE.match(name)
    if not m:
        raise EntryError('name')
    if len(data) < HEADER.size:
        raise EntryError('short')
    magic, version, stage, n, blob_n, reserved, h = HEADER.unpack_from(data)
    if magic != CACHE_MAGIC or version != CACHE_VERSION:
        raise EntryError('header')
    if chr(stage) != m.group(1) or h != int(m.group(2), 16) or n != int(m.group(3), 16):
        raise EntryError('name-mismatch')
    if HEADER.size + n + blob_n != len(data) or not 0 < n <= 256 * 1024 or blob_n <= 0:
        raise EntryError('length')
    source = data[HEADER.size:HEADER.size + n]
    blob = data[HEADER.size + n:]
    if fnv1a(source) != h:
        raise EntryError('source-hash')
    if not d3dc.dxbc_valid(blob):
        raise EntryError('dxbc')
    return dict(name=name, stage=chr(stage), hash=h, source=source, blob=blob)


def compile_args(entry: dict) -> dict:
    """Exactly how the renderer compiles this entry."""
    if entry['stage'] == 'V':
        return dict(entry='main', target='vs_5_0', flags=0, name='xvs', source=entry['source'])
    if entry['stage'] == 'P':
        return dict(entry='main', target='ps_5_0', flags=0, name='xps', source=entry['source'])
    text = entry['source']
    if not text.startswith(BUILTIN_TAG):
        raise EntryError('builtin-tag')
    head, sep, body = text[len(BUILTIN_TAG):].partition(b'\n\n')
    if not sep:
        raise EntryError('builtin-identity')
    fields = dict(line.split(b'=', 1) for line in head.split(b'\n'))
    if int(fields[b'flags2'], 16) != 0:
        raise EntryError('builtin-flags2')
    return dict(entry=fields[b'entry'].decode(), target=fields[b'target'].decode(),
                flags=int(fields[b'flags1'], 16), name=fields[b'name'].decode() or None, source=body)


def serialize_entry(stage: str, h: int, source: bytes, blob: bytes) -> bytes:
    return HEADER.pack(CACHE_MAGIC, CACHE_VERSION, ord(stage), len(source), len(blob), 0, h) + source + blob


def scan_dirs(dirs):
    entries, problems = {}, []
    for d in dirs:
        for p in sorted(Path(d).glob('*.bin')):
            if not NAME_RE.match(p.name):
                continue
            try:
                e = parse_entry(p.name, p.read_bytes())
            except EntryError as x:
                problems.append(dict(file=str(p), problem=str(x)))
                continue
            e['origin'] = str(p)
            prior = entries.get(p.name)
            if prior and prior['source'] != e['source']:
                problems.append(dict(file=str(p), problem='hash-collision-different-source'))
                continue
            if prior:
                prior.setdefault('also', []).append(str(p))
                if prior['blob'] != e['blob']:
                    prior['blob_variants'] = prior.get('blob_variants', 1) + 1
                continue
            entries[p.name] = e
    return entries, problems


# ---------------------------------------------------------------------------
# PSO descriptor records (layout of rsp_pso_record)
# ---------------------------------------------------------------------------
def record_key(raw: bytes):
    return fnv1a(raw, FNV_SEED_HI), fnv1a(raw, FNV_SEED)  # (hi, lo)


def record_shaders(raw: bytes):
    w = struct.unpack('<%dI' % RECORD_WORDS, raw)
    def ref(o):
        stage, n, lo, hi, blen = w[o:o + 5]
        return dict(stage=chr(stage), name=f'{chr(stage)}_{(hi << 32) | lo:016X}_{n:08X}.bin',
                    bytecode_length=blen, digest=struct.pack('<4I', *w[o + 5:o + 9]))
    return ((w[2] << 32) | w[1]), ref(3), ref(12), w[0]


def read_records(path: Path):
    """Returns (roots{hash: bytes}, records{key: raw}, stats). Stops at a torn tail."""
    data = path.read_bytes()
    roots, records = {}, {}
    stats = dict(file=str(path), records=0, roots=0, corrupt_tail=False, duplicates=0)
    if len(data) < 16 or struct.unpack_from('<2I', data) != (FILE_MAGIC, FILE_VERSION):
        stats['invalid'] = True
        return roots, records, stats
    off = 16
    while off + 12 <= len(data):
        kind, n, check = struct.unpack_from('<3I', data, off)
        payload = data[off + 12:off + 12 + n]
        if len(payload) != n:
            stats['corrupt_tail'] = True
            break
        h = fnv1a(payload)
        if ((h ^ (h >> 32)) & 0xFFFFFFFF) != check:
            stats['corrupt_tail'] = True
            break
        if kind == CHUNK_ROOT and n >= 8:
            rh = struct.unpack_from('<Q', payload)[0]
            if fnv1a(payload[8:]) == rh:
                roots[rh] = payload[8:]
        elif kind == CHUNK_PSOD and n == RECORD_BYTES:
            key = record_key(payload)
            if key in records:
                stats['duplicates'] += 1
            records[key] = payload
        off += 12 + n
    stats['records'], stats['roots'] = len(records), len(roots)
    return roots, records, stats


def write_records(path: Path, roots: dict, records: dict) -> bytes:
    out = bytearray(struct.pack('<4I', FILE_MAGIC, FILE_VERSION, 0, 0))
    def chunk(kind, payload):
        h = fnv1a(payload)
        out.extend(struct.pack('<3I', kind, len(payload), (h ^ (h >> 32)) & 0xFFFFFFFF))
        out.extend(payload)
    for rh in sorted(roots):
        chunk(CHUNK_ROOT, struct.pack('<Q', rh) + roots[rh])
    for key in sorted(records):
        chunk(CHUNK_PSOD, records[key])
    path.write_bytes(bytes(out))
    return bytes(out)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def stage_counts(names):
    c = {'V': 0, 'P': 0, 'B': 0}
    for n in names:
        c[n[0]] += 1
    return c


def cmd_scan(a):
    entries, problems = scan_dirs(a.dirs)
    total = sum(len(e['source']) + len(e['blob']) + HEADER.size for e in entries.values())
    result = dict(entries=len(entries), stages=stage_counts(entries), bytes=total, problems=problems,
                  blob_variants=sorted(n for n, e in entries.items() if e.get('blob_variants')))
    print(json.dumps(result, indent=2))


def generator_identity(root: Path):
    out = {}
    for rel in GENERATOR_FILES:
        p = root / rel
        out[rel] = sha256(p.read_bytes()) if p.exists() else None
    return out


def cmd_build(a):
    out = Path(a.out)
    if out.exists() and any(out.iterdir()):
        raise SystemExit('output directory must be new or empty: ' + str(out))
    entries, problems = scan_dirs(a.from_dirs)
    seed = out / SEED_DIR
    seed.mkdir(parents=True)
    manifest_entries, recompile_mismatch = [], []
    for name in sorted(entries):
        e = entries[name]
        args = compile_args(e)
        blob = d3dc.compile_hlsl(args['source'], args['entry'], args['target'], args['flags'], args['name'])
        if not d3dc.dxbc_valid(blob):
            raise SystemExit('compiler produced an invalid container for ' + name)
        if blob != e['blob']:
            recompile_mismatch.append(name)
        data = serialize_entry(e['stage'], e['hash'], e['source'], blob)
        (seed / name).write_bytes(data)
        manifest_entries.append(dict(name=name, stage=e['stage'], bytes=len(data), sha256=sha256(data),
                                     source_bytes=len(e['source']), source_sha256=sha256(e['source']),
                                     dxbc_bytes=len(blob), dxbc_digest=blob[4:20].hex(),
                                     entry=args['entry'], target=args['target'], flags=args['flags'],
                                     runtime_blob_identical=blob == e['blob']))
    # PSO descriptors: keep those whose shaders the seed can supply exactly.
    roots, records, sources = {}, {}, []
    for p in (a.inventory_in or []) + (a.observed or []):
        r, rec, stats = read_records(Path(p))
        roots.update(r)
        records.update(rec)
        sources.append(stats)
    by_name = {m['name']: m for m in manifest_entries}
    kept, dropped = {}, []
    for key, raw in records.items():
        root_hash, vs, ps, version = record_shaders(raw)
        why = None
        if version != 1:
            why = 'record-version'
        elif root_hash not in roots:
            why = 'root-missing'
        else:
            for ref in (vs, ps):
                m = by_name.get(ref['name'])
                if not m:
                    why = 'shader-not-seeded'
                elif m['dxbc_digest'] != ref['digest'].hex() or m['dxbc_bytes'] != ref['bytecode_length']:
                    why = 'shader-bytecode-differs'
                if why:
                    break
        if why:
            dropped.append(dict(key='%016X%016X' % key, reason=why))
        else:
            kept[key] = raw
    used_roots = {rh: roots[rh] for rh in {record_shaders(r)[0] for r in kept.values()}}
    inv_bytes = write_records(out / INVENTORY, used_roots, kept) if kept or a.observed or a.inventory_in else None
    if inv_bytes is None and (out / INVENTORY).exists():
        (out / INVENTORY).unlink()
    reasons = {}
    for d in dropped:
        reasons[d['reason']] = reasons.get(d['reason'], 0) + 1
    manifest = dict(
        format='macross-shader-seed', format_version=3, cache_format_version=CACHE_VERSION,
        pso_record_version=1, pso_record_bytes=RECORD_BYTES,
        compiler=d3dc.compiler_identity(),
        generator=generator_identity(Path(a.source_root)) if a.source_root else None,
        compatibility=('Runtime accepts an entry only when the complete generated HLSL (or built-in identity '
                       'text) matches byte for byte and the DXBC container checksum is valid; anything else '
                       'compiles normally. PSO inventory records are device independent; the pipeline '
                       'library is built locally and bound to adapter vendor/device/subsys/revision + UMD.'),
        entries=manifest_entries, entry_count=len(manifest_entries), stages=stage_counts(by_name),
        seed_bytes=sum(m['bytes'] for m in manifest_entries),
        recompile_differs_from_runtime=recompile_mismatch,
        input_problems=problems,
        pso_inventory=dict(path=INVENTORY if inv_bytes else None, records=len(kept),
                           roots=['%016X' % r for r in sorted(used_roots)],
                           sha256=sha256(inv_bytes) if inv_bytes else None,
                           dropped=len(dropped), dropped_reasons=reasons, inputs=sources),
        coverage_scope=('Observed variants only. Generated shaders depend on guest microcode plus runtime '
                        'texture/render state (formats, cube/shadow/unnormalized sizes, alpha test, sRGB, '
                        'vertex layouts), so the complete set cannot be enumerated offline. This is not the '
                        'complete game.'))
    body = json.dumps(manifest, indent=2, sort_keys=True)
    manifest['manifest_sha256'] = sha256(body.encode())
    (out / MANIFEST).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(json.dumps(dict(out=str(out), entries=len(manifest_entries), stages=manifest['stages'],
                          seed_bytes=manifest['seed_bytes'], recompile_differs=len(recompile_mismatch),
                          input_problems=len(problems), pso_records=len(kept), pso_dropped=reasons,
                          manifest_sha256=manifest['manifest_sha256']), indent=2))


def verify_seed(out, recompile=False):
    """Full check of a build output; returns a result dict with ok/errors (never exits)."""
    out = Path(out)
    manifest = json.loads((out / MANIFEST).read_text(encoding='utf-8'))
    claimed = manifest.pop('manifest_sha256')
    errors = []
    if sha256(json.dumps(manifest, indent=2, sort_keys=True).encode()) != claimed:
        errors.append('manifest digest')
    seed = out / SEED_DIR
    names = {p.name for p in seed.glob('*.bin')}
    listed = {m['name'] for m in manifest['entries']}
    if names != listed:
        errors.append(dict(extra=sorted(names - listed), missing=sorted(listed - names)))
    compiler = d3dc.compiler_identity()
    same_compiler = compiler['sha256'] == manifest['compiler']['sha256']
    rechecked = 0
    for m in manifest['entries']:
        p = seed / m['name']
        if not p.exists():
            continue
        data = p.read_bytes()
        if sha256(data) != m['sha256']:
            errors.append(dict(entry=m['name'], problem='sha256'))
            continue
        try:
            e = parse_entry(m['name'], data)
        except EntryError as x:
            errors.append(dict(entry=m['name'], problem=str(x)))
            continue
        if recompile:
            args = compile_args(e)
            blob = d3dc.compile_hlsl(args['source'], args['entry'], args['target'], args['flags'], args['name'])
            rechecked += 1
            if blob != e['blob'] and same_compiler:
                errors.append(dict(entry=m['name'], problem='recompile-differs-same-compiler'))
    inv = manifest['pso_inventory']
    pso = None
    if inv['path']:
        data = (out / inv['path']).read_bytes()
        if sha256(data) != inv['sha256']:
            errors.append('inventory sha256')
        roots, records, stats = read_records(out / inv['path'])
        unresolved = 0
        for raw in records.values():
            root_hash, vs, ps, _ = record_shaders(raw)
            if root_hash not in roots or vs['name'] not in names or ps['name'] not in names:
                unresolved += 1
        pso = dict(records=len(records), roots=len(roots), unresolved=unresolved, corrupt_tail=stats['corrupt_tail'])
        if unresolved or stats['corrupt_tail'] or len(records) != inv['records']:
            errors.append(dict(inventory=pso))
    result = dict(ok=not errors, entries=len(listed), recompiled=rechecked, same_compiler=same_compiler,
                  compiler=compiler, pso_inventory=pso, errors=errors[:50])
    result['local_only_files'] = sorted(p.name for p in out.rglob('*') if p.name.startswith(LOCAL_ONLY_PREFIXES))
    if result['local_only_files']:
        result['ok'] = False
        result['errors'].append(dict(local_only_files=result['local_only_files']))
    result['manifest_sha256'] = claimed
    return result


def cmd_verify(a):
    result = verify_seed(a.out, a.recompile)
    print(json.dumps(result, indent=2))
    if not result['ok']:
        raise SystemExit(1)


STATS_RE = re.compile(r'\[shader-prep\] stats reason=(\S+) (.*)$')
MISS_RE = re.compile(r'\[shader-miss\] stage=(\w) source=([0-9A-F]+) bytes=(\d+) compile_ms=([\d.]+) frame=(\d+)')
PSO_RE = re.compile(r'\[pso-miss\] key=([0-9A-F]+) known=(\d) library=(\S+) create_ms=([\d.]+) frame=(\d+)')
INIT_RE = re.compile(r'\[shader-prep\] init (.*)$')
HIT_RE = re.compile(r'\[pso-library-hit\] key=([0-9A-F]+) load_ms=([\d.]+)')


def _fields(text):
    out = {}
    for m in re.finditer(r'(\w+)(\{[^}]*\}|=\S+)', text):
        k, v = m.group(1), m.group(2)
        if v.startswith('{'):
            out[k] = {kk: vv for kk, vv in re.findall(r'(\w+)=([^ }]+)', v)}
        else:
            out[k] = v[1:]
    return out


def coverage_from_log(text: str, seed_names=None, from_frame: int = 0):
    init, last = None, None
    misses, psos, hits = [], [], []
    for line in text.splitlines():
        m = INIT_RE.search(line)
        if m:
            init = _fields(m.group(1))
        m = STATS_RE.search(line)
        if m:
            last = _fields(m.group(2))
        m = MISS_RE.search(line)
        if m:
            name = f'{m.group(1)}_{m.group(2)}_{int(m.group(3)):08X}.bin'
            misses.append(dict(name=name, stage=m.group(1), compile_ms=float(m.group(4)), frame=int(m.group(5)),
                               in_seed=(name in seed_names) if seed_names is not None else None))
        m = HIT_RE.search(line)
        if m:
            hits.append(float(m.group(2)))
        m = PSO_RE.search(line)
        if m:
            psos.append(dict(key=m.group(1), known=m.group(2) == '1', library=m.group(3),
                             create_ms=float(m.group(4)), frame=int(m.group(5))))
    late = [x for x in misses if x['frame'] >= from_frame]
    late_pso = [x for x in psos if x['frame'] >= from_frame]
    return dict(
        init=init, final_stats=last,
        shader_compiles=dict(total=len(misses), from_frame=from_frame, after_from_frame=len(late),
                             by_stage={s: sum(1 for x in misses if x['stage'] == s) for s in 'VPB'},
                             compile_ms_total=round(sum(x['compile_ms'] for x in misses), 3),
                             max_ms=max([x['compile_ms'] for x in misses], default=0.0),
                             unexpected_in_seed=[x for x in misses if x['in_seed']],
                             worst=sorted(misses, key=lambda x: -x['compile_ms'])[:12]),
        pso_creates=dict(total=len(psos), after_from_frame=len(late_pso),
                         known=sum(x['known'] for x in psos), unknown=sum(not x['known'] for x in psos),
                         create_ms_total=round(sum(x['create_ms'] for x in psos), 3),
                         max_ms=max([x['create_ms'] for x in psos], default=0.0),
                         note='[pso-miss] lines are capped at 512 per run; final_stats holds full counts'),
        pso_library_hits=dict(total=len(hits), load_ms_total=round(sum(hits), 3), max_ms=max(hits, default=0.0),
                              note='[pso-library-hit] lines are capped at 1024 per run'),
        scope='Counts cover only what this run exercised; unexercised content is not proven covered.')


def cmd_coverage(a):
    names = None
    if a.seed:
        names = {p.name for p in (Path(a.seed) / SEED_DIR).glob('*.bin')}
    text = Path(a.log).read_text(encoding='utf-8', errors='replace')
    result = coverage_from_log(text, names, a.from_frame)
    if a.json:
        Path(a.json).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan'); s.add_argument('dirs', nargs='+'); s.set_defaults(fn=cmd_scan)
    b = sub.add_parser('build')
    b.add_argument('--from', dest='from_dirs', nargs='+', required=True)
    b.add_argument('--observed', nargs='*', default=[])
    b.add_argument('--inventory-in', nargs='*', default=[])
    b.add_argument('--source-root')
    b.add_argument('--out', required=True)
    b.set_defaults(fn=cmd_build)
    v = sub.add_parser('verify'); v.add_argument('out'); v.add_argument('--recompile', action='store_true')
    v.set_defaults(fn=cmd_verify)
    c = sub.add_parser('coverage'); c.add_argument('log'); c.add_argument('--seed'); c.add_argument('--json')
    c.add_argument('--from-frame', type=int, default=0)
    c.set_defaults(fn=cmd_coverage)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == '__main__':
    main()
