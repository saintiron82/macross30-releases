"""Independent offline readback of the menu-title-v10 staged packs; no installation.

1. Byte diff of the staged data.dat / pack.idx against the installed (battle-hud-v9) packs: the
   only differing bytes inside the old length must be the ttl_menutext_01.dds primary record
   fields (master in pack.idx, embedded in data.dat).
2. Readback: the primary ttl_menutext_01.dds decodes to the reviewed v10 candidate, and the
   embedded record mirrors the master record.
3. The candidate still passes the mask proof and the controller-row layout check.
"""
import argparse
import json
from pathlib import Path

from stage_menu_ui_bundle import primary, payload
from stage_ui_images import sha, sha_file, validate_dds
from stage_menu_title_v10 import B, PREV, DISC, HDD, NAME, load, dump, validate_mask
from verify_battle_hud_v9 import diff_offsets

FINAL = {'data.dat': 'staged-primary/data.dat', 'pack.idx': 'staged-primary/pack.idx'}


def verify():
    plan = load(B / 'source-plan.json')
    a = plan['authority']
    new_sha, old_sha = a['replacement_sha256'], a['source_sha256']
    validate_mask(a)
    for e in load(PREV / 'ready.json')['files']:
        if sha_file(DISC / e['filename']) != e['sha256'] or sha_file(HDD / e['filename']) != e['sha256']:
            raise ValueError('Installed v9 epoch changed: ' + e['filename'])
    allowed = {}
    for r in primary(DISC / 'pack.idx')[1]:
        if r['name'] == NAME:
            allowed.setdefault('pack.idx', []).append((r['record'] + 12, 12, 'master primary ' + r['pack']))
            for e in primary(DISC / r['pack'])[1]:
                if e['name'] == NAME and e['pack'] == r['pack']:
                    allowed.setdefault(r['pack'], []).append((e['record'] + 12, 12, 'embedded primary'))
    diffs = {}
    for name, rel in FINAL.items():
        final = B / rel
        if final.stat().st_size < (DISC / name).stat().st_size:
            raise ValueError('Staged pack shorter than source: ' + name)
        offs = diff_offsets(DISC / name, final)
        bad = [o for o in offs if not any(s <= o < s + n for s, n, _ in allowed.get(name, []))]
        if bad:
            raise ValueError(f'{name}: unexpected changed bytes at {bad[:8]}')
        diffs[name] = dict(changed_bytes_in_old_length=len(offs),
                           appended_bytes=final.stat().st_size - (DISC / name).stat().st_size,
                           allowed_fields=[w for _, _, w in allowed.get(name, [])])
    prim = []
    for r in primary(B / FINAL['pack.idx'])[1]:
        if r['name'] != NAME:
            continue
        raw, _ = payload(B / FINAL[r['pack']], r)
        validate_dds(raw)
        if sha(raw) != new_sha:
            raise ValueError('Primary title copy is not the v10 candidate')
        emb = [e for e in primary(B / FINAL[r['pack']])[1] if e['name'] == NAME]
        if len(emb) != 1 or any(emb[0][k] != r[k] for k in ('offset', 'size', 'compressed_size')):
            raise ValueError('Embedded primary mirror mismatch')
        prim.append(dict(pack=r['pack'], offset=r['offset'], sha256=sha(raw)))
    if len(prim) != 1:
        raise ValueError('Expected 1 primary title copy')
    return dict(status='PASS', static_only=True, in_game_verified=False, title_primary=prim,
                title_source_sha256=old_sha, title_replacement_sha256=new_sha, layout_check=a['layout_check'],
                pack_diffs=diffs, staged_hashes={n: sha_file(B / r) for n, r in FINAL.items()},
                source_epoch_unchanged=plan['source_epoch'], unknown_variants=0, active_modified=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=B / 'final-pack-readback.json')
    n = p.parse_args()
    if n.output.exists():
        raise ValueError('Fresh proof path required')
    result = verify()
    dump(n.output, result)
    print('PASS', json.dumps(dict(primary=len(result['title_primary']),
                                  diffs={k: v['changed_bytes_in_old_length'] for k, v in result['pack_diffs'].items()})))


if __name__ == '__main__':
    main()
