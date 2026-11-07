"""Stage two exact nested title DDS replacements over the installed v10 epoch.

Only independent bundle files are written. Active packs/processes are untouched.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from stage_menu_title_v10 import validate_mask
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_menu_ui_bundle import primary
from stage_ui_images import sha, sha_file, bounded_range

ROOT = Path(__file__).resolve().parents[2]
B = ROOT / 'localization/image_assets/title-menu-v11'
PREV = ROOT / 'localization/image_assets/menu-title-v10'
DISC = ROOT / 'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
HDD = ROOT / 'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
PACKS = ('data.dat', 'data2.dat', 'fileset0.dat', 'fileset2.dat', 'pack.idx')
EXPECTED = {
    'title_text.dds': ('53e4815f0c924c78a3baaf14bd2ea7f6b869a8b103c318bd7ea178aa0b37d44e', [[0,0,256,48],[0,48,256,48],[0,96,256,48]], [[0,144,256,112]]),
    'title_logo02.dds': ('4efd619c1bea941f5f8a51be5e39761845810fbad1aa9e7a13faa19fc8de9973', [[0,0,384,64]], []),
}


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, ensure_ascii=False, indent=2)+'\n').encode('utf8'))


def local_path(value, base=ROOT):
    p = Path(value)
    p = (p if p.is_absolute() else ROOT/p).resolve()
    if base.resolve() not in p.parents:
        raise ValueError('Path outside expected tree: '+str(p))
    return p


def epoch_authority():
    # v10/v9 contain changed files only. Resolve the immutable lineage to all five.
    result = {}
    name = 'menu-title-v10'
    seen = set()
    while name:
        if name in seen or Path(name).name != name:
            raise ValueError('Invalid source bundle lineage')
        seen.add(name)
        d = load(ROOT/'localization/image_assets'/name/'ready.json')
        for e in d['files']:
            if e['filename'] in PACKS:
                result.setdefault(e['filename'], e['sha256'])
        name = d.get('previous_bundle', {}).get('name')
    if set(result) != set(PACKS):
        raise ValueError('Incomplete v10 pack epoch')
    return result


def check_epoch(expected):
    for name, digest in expected.items():
        if sha_file(DISC/name) != digest or sha_file(HDD/name) != digest:
            raise ValueError('Installed v10 source changed: '+name)


def authorities(path):
    doc = load(path)
    rows = doc['entries']
    if len(rows) != 2 or {r['name'] for r in rows} != set(EXPECTED):
        raise ValueError('Require exactly two title DDS authorities')
    result = []
    for row in rows:
        a = dict(row)
        digest, rois, protected = EXPECTED[a['name']]
        if a['path'] != 'data/menu/title/'+a['name'] or a['source_sha256'] != digest:
            raise ValueError('Unexpected title source/path')
        if a['ROI_xywh'] != rois or a['protected_xywh'] != protected:
            raise ValueError('Unreviewed title ROI/protection')
        for key in ('source_DDS', 'replacement', 'allowed_pixel_mask'):
            a[key] = str(local_path(a[key], ROOT if key == 'source_DDS' else B))
        a['mask_proof'] = validate_mask(a)
        result.append(a)
    return result


def prepare(authority):
    if load(ROOT/'localization/image_assets/active-ui.json')['bundle'] != 'menu-title-v10':
        raise ValueError('Active source must be menu-title-v10')
    rows = authorities(authority)
    expected = epoch_authority()
    check_epoch(expected)
    _, prim = primary(DISC/'pack.idx')
    if any(r['name'] in EXPECTED for r in prim):
        raise ValueError('Unexpected primary title copy')
    _, archives = pidx(DISC/'pack.idx')
    matches = []
    visited = 0
    by_name = {a['name']: a for a in rows}
    for ar in archives:
        h = region(DISC/ar['pack'], ar['offset'], 20)
        if h[:4] != b'FSTS':
            raise ValueError('Unknown secondary format')
        visited += 1
        count, table, ns, nz = struct.unpack_from('<4I', h, 4)
        bounded_range(ns, nz, ar['size'])
        bounded_range(table, count*16, ar['size'])
        names = region(DISC/ar['pack'], ar['offset']+ns, nz)
        if not any(n.encode() in names for n in EXPECTED):
            continue
        blob = region(DISC/ar['pack'], ar['offset'], ar['size'])
        for r in fsts_entries(blob):
            name = Path(r['path']).name
            if name not in EXPECTED:
                continue
            a = by_name[name]
            raw, _ = decoded(blob, r)
            if ar['path'] != 'title' or ar['pack'] != 'data.dat' or r['path'] != a['path'] or sha(raw) != a['source_sha256']:
                raise ValueError('Unknown title variant/route')
            matches.append(dict(archive=ar, entry=r, source_sha256=sha(raw)))
    if len(matches) != 2 or {m['entry']['path'] for m in matches} != {a['path'] for a in rows}:
        raise ValueError('Missing/duplicate exact title copy')
    elf = PREV/load(PREV/'ready.json')['elf']['path']
    elfsha = load(PREV/'ready.json')['elf']['sha256']
    if sha_file(elf) != elfsha:
        raise ValueError('Prior ELF changed')
    plan = dict(status='PREPARED_EXACT_V10', authority_sha256=sha_file(authority), authorities=rows,
                source_epoch=expected, primary_count=0, matches=matches, FSTS_metadata_visited=visited,
                unknown_variants=0, elf=dict(source=str(elf),sha256=elfsha), active_modified=False)
    dump(B/'source-plan.json', plan)
    dump(B/'nested-extra-map.json', dict(entries=[dict(path=a['path'],original_sha256=a['source_sha256'],
         replacement=a['replacement'],replacement_sha256=a['replacement_sha256']) for a in rows]))
    return plan


def apply(plan):
    out = B/'staged-final'
    artifacts = B/'nested-artifacts'
    if out.exists() or artifacts.exists() or (B/'EBOOT.catalog-pool.elf').exists():
        raise ValueError('Fresh independent output required')
    subprocess.run([sys.executable, str(ROOT/'tools/localization/stage_nested_ui.py'),
                    '--source-dir',str(DISC),'--extra-map',str(B/'nested-extra-map.json'),
                    '--only-extra-map','--output',str(out),'--artifacts',str(artifacts),'--apply'], check=True)
    nested = load(out/'nested-ui-manifest.json')
    if nested['unmatched'] or nested['candidates'] != 2 or len(nested['archives']) != 1:
        raise ValueError('Unexpected nested staging scope')
    for name in PACKS:
        if (out/name).exists():
            continue
        shutil.copyfile(DISC/name,out/name)
        if (out/name).stat().st_nlink != 1 or sha_file(out/name) != plan['source_epoch'][name]:
            raise ValueError('Preserved pack copy mismatch')
    shutil.copyfile(plan['elf']['source'], B/'EBOOT.catalog-pool.elf')
    if sha_file(B/'EBOOT.catalog-pool.elf') != plan['elf']['sha256']:
        raise ValueError('ELF copy mismatch')
    check_epoch(plan['source_epoch'])
    dump(B/'ready-paths.json', dict(files={n:str(out/n) for n in PACKS},
         elf=str(B/'EBOOT.catalog-pool.elf'), source_epoch=plan['source_epoch'], active_modified=False))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--authority',type=Path,default=B/'candidate/authority.json')
    parser.add_argument('--apply',action='store_true')
    args=parser.parse_args()
    plan=prepare(args.authority)
    if args.apply:
        apply(plan)
    print(json.dumps(dict(matches=len(plan['matches']),unknown_variants=0,applied=args.apply)))


if __name__ == '__main__':
    main()
