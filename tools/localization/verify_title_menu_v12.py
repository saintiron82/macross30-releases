"""Independent v12 readback: only one title DDS entry may differ from v11."""
import argparse
import json
import struct
from pathlib import Path

from stage_title_menu_v12 import B, DISC, PACKS, load, dump, authorities, check_epoch
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_ui_images import sha, sha_file, compare_prefix


def verify():
    plan=load(B/'source-plan.json')
    if sha_file(B/'candidate/authority.json') != plan['authority_sha256']:
        raise ValueError('Candidate authority changed after planning')
    rows=authorities(B/'candidate/authority.json')
    by_path={a['path']:a for a in rows}
    check_epoch(plan['source_epoch'])
    candidate=Path(rows[0]['replacement']).read_bytes()
    for y0 in (0,48,96):
        ys=[y for y in range(y0,y0+48) for x in range(256) if candidate[128+4*(y*256+x)+3]>96]
        if not ys or max(ys)-min(ys)+1>22:
            raise ValueError('Refitted title ink exceeds original+2px')
    out=B/'staged-final'
    _, oldars=pidx(DISC/'pack.idx')
    _, newars=pidx(out/'pack.idx')
    if len(oldars)!=len(newars):
        raise ValueError('Archive count changed')
    changed=[]
    for old,new in zip(oldars,newars):
        if old==new:
            continue
        if old['path']!='title' or old['pack']!='data.dat' or any(old[k]!=new[k] for k in ('path','pack','field')):
            raise ValueError('Unselected archive/index changed')
        changed.append((old,new))
    if len(changed)!=1:
        raise ValueError('Exactly one relocated title archive required')
    old,new=changed[0]
    oldblob=region(DISC/old['pack'],old['offset'],old['size'])
    newblob=region(out/new['pack'],new['offset'],new['size'])
    oldrows=fsts_entries(oldblob);newrows=fsts_entries(newblob)
    if len(oldrows)!=len(newrows):
        raise ValueError('FSTS entry count changed')
    allowed=[];readbacks=[];unchanged=[]
    for before,after in zip(oldrows,newrows):
        if before['path']!=after['path'] or before['index']!=after['index'] or before['field']!=after['field']:
            raise ValueError('FSTS name/order changed')
        raw,encoded=decoded(oldblob,before);actual,actual_encoded=decoded(newblob,after)
        a=by_path.get(before['path'])
        if a is None:
            if before!=after or encoded!=actual_encoded or raw!=actual:
                raise ValueError('Protected entry changed: '+before['path'])
            unchanged.append(dict(path=before['path'],sha256=sha(raw),encoded_identical=True))
        else:
            if sha(raw)!=a['source_sha256'] or sha(actual)!=a['replacement_sha256'] or actual!=Path(a['replacement']).read_bytes():
                raise ValueError('Title readback mismatch')
            allowed.extend(range(before['field'],before['field']+12))
            readbacks.append(dict(path=before['path'],sha256=sha(actual),index=after['index']))
    if len(readbacks)!=1:
        raise ValueError('One title DDS replacement required')
    aset=set(allowed)
    if len(newblob)<len(oldblob) or any(a!=b and i not in aset for i,(a,b) in enumerate(zip(oldblob,newblob))):
        raise ValueError('Title archive prefix changed outside selected fields')
    # Exact old pack prefix, including every other asset; only secondary mirror moves.
    _, embedded_old=pidx(DISC/'data.dat')
    _, embedded_new=pidx(out/'data.dat')
    em=[r for r in embedded_old if all(r[k]==old[k] for k in ('path','pack','offset','size'))]
    if len(em)!=1:
        raise ValueError('Embedded secondary source mismatch')
    for n in PACKS:
        if n=='pack.idx':
            compare_prefix(DISC/n,out/n,(DISC/n).stat().st_size,[(old['field'],8)])
            if (out/n).stat().st_size!=(DISC/n).stat().st_size:
                raise ValueError('Master index length changed')
        elif n=='data.dat':
            compare_prefix(DISC/n,out/n,(DISC/n).stat().st_size,[(em[0]['field'],8)])
            if new['offset']!=((DISC/n).stat().st_size+2047)//2048*2048 or (out/n).stat().st_size!=new['offset']+new['size']:
                raise ValueError('Unexpected appended pack bytes')
            mirrors=[r for r in embedded_new if r['field']==em[0]['field']]
            if len(mirrors)!=1 or any(mirrors[0][k]!=new[k] for k in ('path','pack','offset','size')):
                raise ValueError('Embedded/master mirror mismatch')
        elif sha_file(out/n)!=plan['source_epoch'][n]:
            raise ValueError('Preserved backing pack changed: '+n)
    if sha_file(B/'EBOOT.catalog-pool.elf')!=plan['elf']['sha256']:
        raise ValueError('ELF changed')
    return dict(status='PASS',static_only=True,in_game_verified=False,active_modified=False,
                source_epoch_unchanged=plan['source_epoch'],primary_changed=0,
                nested_title_readbacks=readbacks,protected_title_entries=unchanged,
                all_old_pack_bytes_preserved_except_secondary_mirror_fields=True,
                archive_prefix_preserved_except_selected_entry_fields=True,
                staged_hashes={n:sha_file(out/n) for n in PACKS},ELFsha256=plan['elf']['sha256'],unknown_variants=0)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=B/'final-pack-readback.json')
    args=p.parse_args()
    if args.output.exists():
        raise ValueError('Fresh proof required')
    result=verify();dump(args.output,result)
    print('PASS title DDS1 / all other source assets preserved')


if __name__=='__main__':
    main()
