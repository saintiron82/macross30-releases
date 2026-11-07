"""Read-only: extract every primary and nested (FSTS) copy of named DDS files from
the installed disc/HDD packs, record SHA-256 per copy and save black-background
previews. Never writes pack data and never touches saves.

Example:
  python tools/localization/audit_installed_textures.py --out analysis/localization/x \
      placename_t04_title01.dds hud_cockpit_01.dds
"""
import argparse
import hashlib
import io
import json
import struct
from pathlib import Path

import sys as _sys, pathlib as _pl
_sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]
from stage_menu_ui_bundle import primary, payload
from stage_nested_ui import pidx, region, fsts_entries, decoded

ROOT = Path(__file__).resolve().parents[2]
ROLES = {
    'disc': ROOT / 'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK',
    'hdd': ROOT / 'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack',
}


def sha(b):
    return hashlib.sha256(b).hexdigest()


def preview(raw, path):
    from PIL import Image
    h, w = struct.unpack_from('<II', raw, 12)
    if len(raw) != 128 + w * h * 4:
        return False
    im = Image.frombuffer('RGBA', (w, h), raw[128:], 'raw', 'BGRA', 0, 1)
    bg = Image.new('RGBA', (w, h), (0, 0, 0, 255))
    bg.alpha_composite(im)
    bg.convert('RGB').save(path)
    return True


def locate(folder, name):
    """Pack file by case-insensitive name; HDD install lacks some packs (falls back to disc)."""
    for base in (folder, ROLES['disc']):
        for p in base.iterdir():
            if p.name.lower() == name.lower():
                return p
    raise FileNotFoundError(name)


def audit(names, out, roles=ROLES):
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    seen = {}
    for role, folder in roles.items():
        _, prim = primary(folder / 'pack.idx')
        for r in prim:
            if r['name'] not in names:
                continue
            raw, _ = payload(locate(folder, r['pack']), r)
            rows.append(dict(role=role, kind='primary', name=r['name'], pack=r['pack'], offset=r['offset'],
                             record=r['record'], sha256=sha(raw)))
            seen.setdefault((r['name'], sha(raw)), raw)
        _, archives = pidx(folder / 'pack.idx')
        for ar in archives:
            pack = locate(folder, ar['pack'])
            header = region(pack, ar['offset'], 20)
            if header[:4] != b'FSTS':
                continue
            count, table, ns, nsz = struct.unpack_from('<4I', header, 4)
            blob = region(pack, ar['offset'] + ns, nsz)
            if not any(n.encode() in blob for n in names):
                continue
            archive = region(pack, ar['offset'], ar['size'])
            for entry in fsts_entries(archive):
                base = Path(entry['path']).name
                if base not in names:
                    continue
                raw, _ = decoded(archive, entry)
                rows.append(dict(role=role, kind='nested', name=base, archive=ar['path'], entry=entry['path'],
                                 sha256=sha(raw)))
                seen.setdefault((base, sha(raw)), raw)
    variants = []
    for (name, h), raw in seen.items():
        png = out / f'{Path(name).stem}-{h[:12]}-black.png'
        variants.append(dict(name=name, sha256=h, preview=str(png.relative_to(ROOT)) if preview(raw, png) else None))
    return dict(status='READ_ONLY_INSTALLED_AUDIT', names=sorted(names), copies=rows, variants=variants)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('names', nargs='+')
    a = p.parse_args()
    out = a.out if a.out.is_absolute() else ROOT / a.out
    result = audit(set(a.names), out)
    (out / 'installed-audit.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    for v in result['variants']:
        n = sum(1 for c in result['copies'] if c['name'] == v['name'] and c['sha256'] == v['sha256'])
        print(v['name'], v['sha256'][:12], 'copies', n, v['preview'])


if __name__ == '__main__':
    main()
