"""Independent offline readback of the battle-hud-v9 staged packs; no installation.

1. Byte diff of every staged pack against the installed v8 pack: the only
   differing bytes inside the old length must be index fields of the
   hud_cockpit_01.dds primary record (master + embedded) and of the secondary
   archives that hold a nested hud_cockpit_01.dds copy.
2. Readback: every hud_cockpit_01.dds copy (primary + nested) decodes to the
   reviewed v9 candidate; every other entry of a rebuilt archive decodes to the
   same bytes as in the installed v8 archive.
"""
import argparse
import json
import struct
from pathlib import Path

from stage_menu_ui_bundle import primary, payload
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_ui_images import sha, sha_file, validate_dds
from stage_battle_hud_v9 import B, V8, DISC, HDD, load, dump

NAME = 'hud_cockpit_01.dds'
FINAL = {'data2.dat': 'staged-primary/data2.dat', 'data.dat': 'nested-staged/data.dat',
         'fileset0.dat': 'nested-staged/fileset0.dat', 'pack.idx': 'nested-staged/pack.idx'}


def diff_offsets(old, new, limit=4096):
    out = []
    with open(old, 'rb') as f, open(new, 'rb') as g:
        pos = 0
        while True:
            a = f.read(8 << 20)
            if not a:
                break
            b = g.read(len(a))
            if a != b:
                for i in range(len(a)):
                    if a[i] != b[i]:
                        out.append(pos + i)
                        if len(out) > limit:
                            raise ValueError('Too many differing bytes in ' + str(new))
            pos += len(a)
    return out


def allowed_fields(folder):
    """{filename: [(start, length, why)]} index fields that may legitimately change."""
    allowed = {}
    _, master = primary(folder / 'pack.idx')
    for r in master:
        if r['name'] == NAME:
            allowed.setdefault('pack.idx', []).append((r['record'] + 12, 12, 'master primary ' + r['pack']))
            _, embedded = primary(folder / r['pack'])
            for e in embedded:
                if e['name'] == NAME and e['pack'] == r['pack']:
                    allowed.setdefault(r['pack'], []).append((e['record'] + 12, 12, 'embedded primary'))
    _, archives = pidx(folder / 'pack.idx')
    for ar in archives:
        pack = folder / ar['pack']
        head = region(pack, ar['offset'], 20)
        if head[:4] != b'FSTS':
            continue
        count, table, ns, nsz = struct.unpack_from('<4I', head, 4)
        if NAME.encode() not in region(pack, ar['offset'] + ns, nsz):
            continue
        allowed.setdefault('pack.idx', []).append((ar['field'], 8, 'master secondary ' + ar['path']))
        for e in pidx(pack)[1]:
            if all(e[k] == ar[k] for k in ('path', 'pack', 'offset', 'size')):
                allowed.setdefault(ar['pack'], []).append((e['field'], 8, 'embedded secondary ' + ar['path']))
    return allowed, archives


def verify():
    plan = load(B / 'source-plan.json')
    new_sha = plan['authority']['replacement_sha256']
    old_sha = plan['authority']['source_sha256']
    for e in load(V8 / 'ready.json')['files']:
        if sha_file(DISC / e['filename']) != e['sha256'] or sha_file(HDD / e['filename']) != e['sha256']:
            raise ValueError('Installed v8 epoch changed: ' + e['filename'])
    allowed, old_archives = allowed_fields(DISC)
    diffs = {}
    for name, rel in FINAL.items():
        final = B / rel
        if final.stat().st_size < (DISC / name).stat().st_size:
            raise ValueError('Staged pack shorter than source: ' + name)
        offs = diff_offsets(DISC / name, final)
        bad = [o for o in offs if not any(s <= o < s + n for s, n, _ in allowed.get(name, []))]
        if bad:
            raise ValueError(f'{name}: unexpected changed bytes at {bad[:8]}')
        diffs[name] = dict(changed_bytes_in_old_length=len(offs), appended_bytes=final.stat().st_size - (DISC / name).stat().st_size,
                           allowed_fields=[w for _, _, w in allowed.get(name, [])])

    def pack_path(n):
        return B / FINAL[n] if n in FINAL else DISC / n
    _, master = primary(B / FINAL['pack.idx'])
    prim = []
    for r in master:
        if r['name'] != NAME:
            continue
        raw, _ = payload(pack_path(r['pack']), r)
        validate_dds(raw)
        if sha(raw) != new_sha:
            raise ValueError('Primary HUD copy is not the v9 candidate')
        emb = [e for e in primary(pack_path(r['pack']))[1] if e['name'] == NAME]
        if len(emb) != 1 or any(emb[0][k] != r[k] for k in ('offset', 'size', 'compressed_size')):
            raise ValueError('Embedded primary mirror mismatch')
        prim.append(dict(pack=r['pack'], offset=r['offset'], sha256=sha(raw)))
    _, archives = pidx(B / FINAL['pack.idx'])
    old_by_path = {a['path']: a for a in old_archives}
    nested = []
    for ar in archives:
        pack = pack_path(ar['pack'])
        head = region(pack, ar['offset'], 20)
        if head[:4] != b'FSTS':
            continue
        count, table, ns, nsz = struct.unpack_from('<4I', head, 4)
        if NAME.encode() not in region(pack, ar['offset'] + ns, nsz):
            continue
        new_blob = region(pack, ar['offset'], ar['size'])
        old = old_by_path[ar['path']]
        old_blob = region(DISC / old['pack'], old['offset'], old['size'])
        new_entries, old_entries = fsts_entries(new_blob), fsts_entries(old_blob)
        if [e['path'] for e in new_entries] != [e['path'] for e in old_entries]:
            raise ValueError('Archive entry list changed: ' + ar['path'])
        same = 0
        for ne, oe in zip(new_entries, old_entries):
            nraw, _ = decoded(new_blob, ne)
            oraw, _ = decoded(old_blob, oe)
            if Path(ne['path']).name == NAME:
                if sha(oraw) != old_sha or sha(nraw) != new_sha:
                    raise ValueError('Nested HUD copy mismatch in ' + ar['path'])
                nested.append(dict(archive=ar['path'], path=ne['path'], sha256=sha(nraw)))
            elif nraw != oraw:
                raise ValueError('Unrelated nested entry changed: ' + ne['path'])
            else:
                same += 1
        nested[-1]['other_entries_identical'] = same
    if len(prim) != 1 or len(nested) != 4:
        raise ValueError('Expected 1 primary + 4 nested HUD copies')
    return dict(status='PASS', static_only=True, in_game_verified=False, HUD_primary=prim, HUD_nested=nested,
                HUD_source_sha256=old_sha, HUD_replacement_sha256=new_sha, pack_diffs=diffs,
                staged_hashes={n: sha_file(B / r) for n, r in FINAL.items()},
                source_epoch_unchanged=plan['source_epoch'], unknown_variants=0, active_modified=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=B / 'final-pack-readback.json')
    n = p.parse_args()
    if n.output.exists():
        raise ValueError('Fresh proof path required')
    result = verify()
    dump(n.output, result)
    print('PASS', json.dumps(dict(primary=len(result['HUD_primary']), nested=len(result['HUD_nested']),
                                  diffs={k: v['changed_bytes_in_old_length'] for k, v in result['pack_diffs'].items()})))


if __name__ == '__main__':
    main()
