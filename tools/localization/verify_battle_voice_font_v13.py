"""Independent staged-pack readback of the two resident v13 font payloads."""
import argparse
from pathlib import Path

from stage_battle_voice_font_v13 import B, DISC, PACKS, ABF, DDS, OLD_SHA, NEW_SHA, load, dump, check_epoch, validate_font, coverage
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_ui_images import sha, sha_file, compare_prefix


def verify():
    plan=load(B/'source-plan.json');out=B/'staged-final'
    if sha_file(B/'candidate/authority.json')!=plan['authority_sha256']:raise ValueError('Font authority changed')
    check_epoch(plan['source_epoch'])
    _,oldars=pidx(DISC/'pack.idx');_,newars=pidx(out/'pack.idx')
    if len(oldars)!=len(newars):raise ValueError('Archive count changed')
    moved=[]
    for old,new in zip(oldars,newars):
        if old==new:continue
        if old['path']!='resident' or old['pack']!='data.dat' or any(old[k]!=new[k] for k in ('path','pack','field')):
            raise ValueError('Non-font secondary route changed')
        moved.append((old,new))
    if len(moved)!=1:raise ValueError('Exactly one resident relocation required')
    old,new=moved[0];original=region(DISC/old['pack'],old['offset'],old['size']);staged=region(out/new['pack'],new['offset'],new['size'])
    before=fsts_entries(original);after=fsts_entries(staged)
    if len(before)!=len(after):raise ValueError('Resident entry count changed')
    selected_paths={'data/font/'+n for n in OLD_SHA};allowed=set();preserved=[];readbacks=[];old_assets={};new_assets={}
    for source,row in zip(before,after):
        if any(source[k]!=row[k] for k in ('path','index','field')):raise ValueError('Resident names/order changed')
        raw,encoded=decoded(original,source);actual,actual_encoded=decoded(staged,row)
        if source['path'] in selected_paths:
            name=Path(source['path']).name
            if sha(raw)!=OLD_SHA[name] or sha(actual)!=NEW_SHA[name] or actual!=(B/'candidate'/name).read_bytes():
                raise ValueError('Resident font payload readback differs')
            allowed.update(range(source['field'],source['field']+12));old_assets[name]=raw;new_assets[name]=actual
            readbacks.append(dict(path=row['path'],index=row['index'],source_sha256=sha(raw),replacement_sha256=sha(actual)))
        else:
            if source!=row or raw!=actual or encoded!=actual_encoded:raise ValueError('Other resident asset changed: '+source['path'])
            preserved.append(dict(path=row['path'],sha256=sha(raw),encoded_byte_identical=True))
    if len(readbacks)!=2 or set(old_assets)!=set(OLD_SHA):raise ValueError('ABF/DDS pair missing')
    if len(staged)<len(original) or any(a!=b and i not in allowed for i,(a,b) in enumerate(zip(original,staged))):
        raise ValueError('Resident prefix changed outside two font entry fields')
    font=validate_font(old_assets[ABF],old_assets[DDS],new_assets[ABF],new_assets[DDS]);fontcoverage=coverage(new_assets[ABF])
    _,embedded_old=pidx(DISC/'data.dat');_,embedded_new=pidx(out/'data.dat')
    original_mirror=[r for r in embedded_old if all(r[k]==old[k] for k in ('path','pack','offset','size'))]
    if len(original_mirror)!=1:raise ValueError('Original embedded resident mirror missing')
    diffs={}
    for name in PACKS:
        original_size=(DISC/name).stat().st_size;new_size=(out/name).stat().st_size
        if name=='pack.idx':
            compare_prefix(DISC/name,out/name,original_size,[(old['field'],8)])
            if new_size!=original_size:raise ValueError('Master index length changed')
            diffs[name]=dict(allowed_changed_fields=[[old['field'],8]],appended_bytes=0)
        elif name=='data.dat':
            compare_prefix(DISC/name,out/name,original_size,[(original_mirror[0]['field'],8)])
            if new['offset']!=(original_size+2047)//2048*2048 or new_size!=new['offset']+new['size']:
                raise ValueError('Unexpected appended DATA bytes')
            mirror=[r for r in embedded_new if r['field']==original_mirror[0]['field']]
            if len(mirror)!=1 or any(mirror[0][k]!=new[k] for k in ('path','pack','offset','size')):
                raise ValueError('Embedded/master resident mirror differs')
            diffs[name]=dict(allowed_changed_fields=[[original_mirror[0]['field'],8]],appended_bytes=new_size-original_size)
        else:
            if sha_file(out/name)!=plan['source_epoch'][name] or new_size!=original_size:raise ValueError('Preserved backing changed: '+name)
            diffs[name]=dict(whole_file_byte_identical=True,appended_bytes=0)
    if sha_file(B/'EBOOT.catalog-pool.elf')!=plan['elf']['sha256']:raise ValueError('ELF changed')
    check_epoch(plan['source_epoch'])
    return dict(status='PASS',static_only=True,in_game_verified=False,active_modified=False,primary_changed=0,
                source_epoch_unchanged=plan['source_epoch'],resident_font_readbacks=readbacks,
                all_other_resident_entries_preserved=preserved,
                all_old_pack_bytes_preserved_except_secondary_mirror_fields=True,
                resident_prefix_preserved_except_two_font_entry_fields=True,
                font_preservation=font,font_coverage=fontcoverage,pack_diffs=diffs,
                staged_hashes={n:sha_file(out/n) for n in PACKS},ELFsha256=plan['elf']['sha256'],unknown_variants=0,
                scope='Resident ABF/DDS only; every v12 menu/subtitle/GOP/image remains in exact preserved pack prefix')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=B/'final-pack-readback.json');args=parser.parse_args()
    if args.output.exists() or B.resolve() not in args.output.resolve().parents:raise ValueError('Fresh bundle-local proof path required')
    result=verify();dump(args.output,result);print('PASS resident font2 / 8177glyphs / KO413 covered / all other v12 assets preserved')
