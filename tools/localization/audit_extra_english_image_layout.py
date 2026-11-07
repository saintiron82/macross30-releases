"""Check extra English glyph bounds against original ARK TXOS rectangles."""
import json
import argparse
import struct
from pathlib import Path
import numpy as np
from english_pack import ROOT,digest,write
from encode_misc_menu_replacements import pixels

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--folder',type=Path,default=ROOT/'localization/en-US/images/extra-ui-v1');args=parser.parse_args()
    folder=args.folder.resolve();plan=json.loads((folder/'composition-plan.json').read_text(encoding='utf8'))
    names={a['name'] for a in plan['assets']};refs={name:[] for name in names}
    for path in (ROOT/'analysis/ui_assets').glob('*.ark'):
        data=path.read_bytes()
        if not any(n.encode() in data for n in names):continue
        at=0;textures=[]
        while at+16<=len(data):
            magic,tag,size,_=struct.unpack_from('<4s4sII',data,at)
            if magic!=b'ARK ' or at+16+size>len(data):raise ValueError(f'Invalid ARK chunk: {path.name} at {at:#x}, {magic!r}, size {size}, file {len(data)}')
            if tag==b'EOF ':break
            if tag in (b'TEXS',b'TXOS'):
                count,stride,offset,_=struct.unpack_from('<4I',data,at+16)
                if offset+count*stride>size:raise ValueError('Invalid ARK table')
                if tag==b'TEXS':textures=[data[at+16+offset+i*stride:at+16+offset+(i+1)*stride].split(b'\0',1)[0].decode('ascii') for i in range(count)]
                else:
                    for i in range(count):
                        f=struct.unpack_from('<12i',data,at+16+offset+i*stride)
                        if 0<=f[2]<len(textures) and textures[f[2]] in names:
                            refs[textures[f[2]]].append(dict(ark=str(path.relative_to(ROOT)),ark_sha256=digest(data),txos=i,rect=list(f[3:7])))
                    break
            at+=16+size
    proofs=[];unused=[];failures=[]
    for a in plan['assets']:
        decoded=np.asarray(pixels(Path(a['replacement']).read_bytes(),a['width'],a['height']),dtype=np.uint8).reshape(a['height'],a['width'],4)
        for row in a['rows']:
            x,y,w,h=row['rect'];region=decoded[y:y+h,x:x+w]
            mask=region[:,:,3]>32
            if a['mode']=='panel':mask &= (region[:,:,:3]>max(64,int(min(a['color'])*0.8))).all(axis=2)
            pos=np.argwhere(mask)
            if not len(pos):failures.append(dict(name=a['name'],text=row['text'],reason='no visible English glyphs'));continue
            box=[int(pos[:,1].min())+x,int(pos[:,0].min())+y,int(pos[:,1].max())+x+1,int(pos[:,0].max())+y+1]
            overlapping=[r for r in refs[a['name']] if r['rect'][1]<y+h and r['rect'][1]+r['rect'][3]>y]
            matching=[r for r in overlapping if r['rect'][0]<=box[0] and r['rect'][1]<=box[1] and r['rect'][0]+r['rect'][2]>=box[2] and r['rect'][1]+r['rect'][3]>=box[3]]
            proof=dict(name=a['name'],text=row['text'],ink_bounds=box,matching_TXOS=matching)
            if matching:proofs.append(proof)
            elif not overlapping:unused.append(dict(**proof,review='No reference for this row in extracted ARKs; runtime use not established'))
            else:failures.append(dict(**proof,overlapping_TXOS=overlapping,reason='glyphs escape source UV rectangle'))
    write(folder/'layout-audit.json',dict(status='PASS' if not failures else 'FAIL',verified_labels=proofs,unreferenced_rows=unused,failures=failures,game_render_verified=False))
    print(json.dumps(dict(verified_labels=len(proofs),unreferenced_rows=len(unused),failures=len(failures))))
    if failures:raise ValueError('Extra image TXOS layout failed')
