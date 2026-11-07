"""Package all unambiguous higher-resolution GB maps referenced by paired MB models."""
import hashlib
import io
import json
import re
import struct
import sys
from pathlib import Path
from PIL import Image
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/localization'))
from stage_gop_primary_mirrors import pidx, read_payload

def fnv(raw):
    n = 14695981039346656037
    for b in raw:
        n = ((n ^ b) * 1099511628211) & 0xffffffffffffffff
    return n

def canonical(name):
    return re.sub('_', '', name[2:].lower())

def main():
    pack = ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
    _, rows, _ = pidx(pack/'pack.idx')
    index = {r['name'].lower():r for r in rows}
    files = {p.name.lower():p for p in pack.iterdir() if p.is_file()}
    cache = {}
    def read(name):
        if name not in cache:
            r = index[name]; cache[name] = read_payload(files[Path(r['pack']).name.lower()], r)[0]
        return cache[name]
    def refs(model):
        return sorted(set(x.decode().lower() for x in re.findall(rb'[A-Za-z0-9_]+\.dds',read(model))))
    def layout(raw):
        if raw[:4] != b'DDS ' or raw[84:88] not in (b'DXT1',b'DXT3',b'DXT5'):
            return None
        return struct.unpack_from('<II', raw, 12), raw[84:88]
    models = json.loads((ROOT/'analysis/aircraft-texture-comparison/manifest.json').read_text(encoding='utf8'))['models']
    candidates = {}; skipped = []; unchanged = 0
    for m in models:
        battle = refs(m['battle']['model']); gallery = refs(m['gallery']['model'])
        for source in battle:
            if source in gallery:
                unchanged += 1
                continue
            options = [n for n in gallery if n.startswith('gb') and canonical(n)==canonical(source)]
            # VF-0A's battle diffuse omits the gallery's _00 suffix.
            if not options and source == 'mb_00f.dds' and 'gb_00f_00.dds' in gallery:
                options = ['gb_00f_00.dds']
            if len(options)!=1 or source not in index or options[0] not in index:
                skipped.append(dict(model=m['key'], source=source, reason='no unique named gallery counterpart'))
                continue
            target = options[0]; a=read(source); b=read(target); la=layout(a); lb=layout(b)
            if not la or not lb or la[1]!=lb[1] or la[0][0]!=la[0][1] or lb[0][0]!=lb[0][1] or not la[0][0]<=lb[0][0]<=la[0][0]*4:
                skipped.append(dict(model=m['key'], source=source, target=target, reason='incompatible format or resolution'))
                continue
            row=candidates.setdefault(source,dict(targets={}, models=[]))
            row['targets'][hashlib.sha256(b).hexdigest()] = target
            row['models'].append(m['key'])
    accepted=[]; body=bytearray(); conflicts=[]
    for source,row in sorted(candidates.items()):
        if len(row['targets'])!=1:
            conflicts.append(dict(source=source, **row)); continue
        target=next(iter(row['targets'].values()));a=read(source);b=read(target)
        x=Image.open(io.BytesIO(a)).convert('RGB');y=Image.open(io.BytesIO(b)).convert('RGB').resize(x.size,Image.Resampling.BOX)
        mae=float(np.abs(np.asarray(x,dtype=np.int16)-np.asarray(y,dtype=np.int16)).mean())
        body.extend(struct.pack('<II',len(a),len(b)));body.extend(a);body.extend(b)
        accepted.append(dict(source=source,target=target,models=sorted(set(row['models'])),kind='normal' if source.endswith('_n.dds') else 'color_or_auxiliary',
                             source_size=list(x.size),target_size=list(Image.open(io.BytesIO(b)).size),
                             source_sha256=hashlib.sha256(a).hexdigest(),target_sha256=hashlib.sha256(b).hexdigest(),
                             downsample_rgb_mae=mae,live_verified=source=='mb_00f.dds'))
    raw=b'M30HD001'+struct.pack('<I',len(accepted))+body
    out=ROOT/'native/assets';out.mkdir(exist_ok=True)
    (out/'aircraft-hd.pack').write_bytes(raw)
    manifest=dict(version=1,source='Original game MB/GB model references; no generated art',models_scanned=len(models),
                  unique_replacements=len(accepted),unchanged_reference_count=unchanged,entries=accepted,skipped=skipped,conflicts=conflicts,
                  pack_sha256=hashlib.sha256(raw).hexdigest(),pack_bytes=len(raw),all_craft_live_verified=False,
                  warning='Name/model-reference correspondence and image comparison are offline evidence, not complete mesh UV or live combat verification.')
    (out/'aircraft-hd-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
    # A compiled pin prevents accidentally substituting a different texture pack.
    header=ROOT/'vendor/ps3recomp-master/libs/video/aircraft_hd_pack_pin.h'
    header.write_text('#define AIRCRAFT_HD_PACK_FNV UINT64_C(0x%016x)\n#define AIRCRAFT_HD_PACK_BYTES %du\n#define AIRCRAFT_HD_PACK_COUNT %du\n'%(fnv(raw),len(raw),len(accepted)),encoding='ascii')
    print(json.dumps({k:manifest[k] for k in ('models_scanned','unique_replacements','pack_bytes')}))

if __name__=='__main__':main()
