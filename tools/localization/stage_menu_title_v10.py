"""menu-title-v10: stage the refitted ttl_menutext_01.dds on top of the installed battle-hud-v9 packs.

Offline exact-SHA staging; never writes active data, never controls the game. Source epoch =
the installed battle-hud-v9 packs (disc and HDD must both match v9 ready.json).
ttl_menutext_01.dds has one primary record (data.dat) and no nested FSTS copy (checked here).

  python tools/localization/stage_menu_title_v10.py --authority localization/image_assets/menu-title-v10/candidate/authority.json [--apply]
"""
import argparse
import json
import struct
import sys
from pathlib import Path

from stage_menu_ui_bundle import primary, payload, make_plan, choose
from stage_nested_ui import pidx, region
import stage_ui_images as ui

ROOT = Path(__file__).resolve().parents[2]
B = ROOT / 'localization/image_assets/menu-title-v10'
PREV = ROOT / 'localization/image_assets/battle-hud-v9'
DISC = ROOT / 'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
HDD = ROOT / 'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
NAME = 'ttl_menutext_01.dds'


def load(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))


def dump(p, d):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(d, ensure_ascii=False, indent=2), encoding='utf8')


def inside(x, y, rects):
    return any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in rects)


def validate_mask(a):
    old = Path(a['source_DDS']).read_bytes()
    new = Path(a['replacement']).read_bytes()
    mask = Path(a['allowed_pixel_mask']).read_bytes()
    ui.validate_dds(old)
    ui.validate_dds(new)
    if ui.sha(old) != a['source_sha256'] or ui.sha(mask) != a['allowed_mask_sha256'] or ui.sha(new) != a['replacement_sha256']:
        raise ValueError('Title source/mask/candidate hash changed')
    h, w = struct.unpack_from('<II', old, 12)
    if len(old) != len(new) or old[:128] != new[:128] or len(old) != 128 + w * h * 4 or len(mask) != w * h:
        raise ValueError('Title header/dimensions/mask invalid')
    changed = 0
    for n, m in enumerate(mask):
        x, y = n % w, n // w
        if m not in (0, 1):
            raise ValueError('Invalid allowed mask value')
        if m and not inside(x, y, a['ROI_xywh']):
            raise ValueError('Mask escapes reviewed title cells')
        if m and inside(x, y, a['protected_xywh']):
            raise ValueError('Mask overlaps protected sprite')
        delta = old[128 + n * 4:132 + n * 4] != new[128 + n * 4:132 + n * 4]
        if delta and not m:
            raise ValueError('RGBA changed outside mask')
        changed += bool(delta)
    for x, y, ww, hh in a['protected_xywh']:
        for yy in range(y, y + hh):
            q = 128 + (yy * w + x) * 4
            if old[q:q + ww * 4] != new[q:q + ww * 4]:
                raise ValueError('Protected sprite changed')
    if not changed:
        raise ValueError('Empty title patch')
    return dict(header_changed_bytes=0, outside_mask_RGBA_changed_pixels=0, protected_RGBA_changed_bytes=0,
                changed_pixels=changed, mask_sha256=ui.sha(mask))


def prepare(authority, source=DISC, hdd=HDD):
    a = load(authority)
    if a['name'] != NAME:
        raise ValueError('Single exact title authority required')
    replacement = Path(a['replacement']).resolve()
    raw = replacement.read_bytes()
    ui.validate_dds(raw)
    if ui.sha(raw) != a['replacement_sha256']:
        raise ValueError('Replacement changed')
    maskproof = validate_mask(a)
    a = dict(a, replacement=str(replacement))
    epoch = []
    for e in load(PREV / 'ready.json')['files']:
        name = e['filename']
        h = ui.sha_file(source / name)
        if h != e['sha256'] or ui.sha_file(hdd / name) != h:
            raise ValueError('Installed v9 source epoch differs ' + name)
        epoch.append(dict(filename=name, sha256=h, disc=str(source / name), hdd=str(hdd / name)))
    for name in ('data.dat', 'pack.idx'):
        if ui.sha_file(source / name) != ui.sha_file(hdd / name):
            raise ValueError('Disc/HDD differ: ' + name)
    _, rows = primary(source / 'pack.idx')
    selected = [r for r in rows if r['name'] == NAME]
    if len(selected) != 1:
        raise ValueError('Expected exactly one title primary record')
    records = []
    for r in selected:
        original, _ = payload(source / r['pack'], r)
        choose([a], r['name'], original)
        records.append(dict(r, raw_sha256=ui.sha(original)))
    _, archives = pidx(source / 'pack.idx')
    for ar in archives:
        pack = source / ar['pack']
        header = region(pack, ar['offset'], 20)
        if header[:4] != b'FSTS':
            raise ValueError('Unknown secondary archive')
        count, table, ns, nsz = struct.unpack_from('<4I', header, 4)
        ui.bounded_range(ns, nsz, ar['size'])
        if b'ttl_menutext_01' in region(pack, ar['offset'] + ns, nsz):
            raise ValueError('Unexpected nested title copy in ' + ar['path'])
    result = dict(status='EXACT_V9_SOURCE_PREPARED', authority=a, candidate_mask_proof=maskproof, source_epoch=epoch,
                  primary=records, nested=[], metadata_archives=len(archives), unknown_variants=0,
                  active_modified=False)
    dump(B / 'source-plan.json', result)
    return result


def apply(plan):
    out = B / 'staged-primary'
    if out.exists():
        raise ValueError('Require fresh primary output')
    out.mkdir(parents=True)
    idx = DISC / 'pack.idx'
    stages = []
    for n, name in enumerate(sorted({r['pack'] for r in plan['primary']})):
        p = make_plan(DISC / name, idx, [plan['authority']], ui.sha_file(DISC / name), ui.sha_file(idx))
        result = ui.apply(p, out / f'stage-{n:02d}')
        (out / f'stage-{n:02d}' / name).rename(out / name)
        idx = out / f'stage-{n:02d}' / 'pack.idx'
        stages.append(result)
    idx.rename(out / 'pack.idx')
    dump(out / 'primary-bundle-result.json', dict(stages=stages, active_modified=False))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--authority', type=Path, required=True)
    p.add_argument('--apply', action='store_true')
    n = p.parse_args()
    plan = prepare(n.authority)
    if n.apply:
        apply(plan)
    print(json.dumps(dict(primary=len(plan['primary']), nested=0, unknown_variants=0, applied=n.apply)))


if __name__ == '__main__':
    main()
