"""Independent v13 resident-font staging over installed title-menu-v12.

Writes bundle-local candidate packs only. No selection, installation or process
control. All five disc/HDD source pack hashes must match reviewed v12 lineage.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from audit_runtime_font import chunks, read_font
from build_korean_font import decode_alpha
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_menu_ui_bundle import primary
from stage_ui_images import sha, sha_file, bounded_range

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/image_assets/battle-voice-font-v13'
PREV=ROOT/'localization/image_assets/title-menu-v12'
DISC=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
HDD=ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
PACKS=('data.dat','data2.dat','fileset0.dat','fileset2.dat','pack.idx')
ABF='font_j24x24.abf';DDS='font_j24x24_0.dds'
OLD_SHA={ABF:'1d5ba9ee7c35a254a4a8a5d50e89e57d4c3025eeceaa3bd7a1d97ccb8628046c',DDS:'1f5f724d77b7ae8a9e50700ee6f86d884ac5113689f4ad9b7e1a4a42de34d5a1'}
NEW_SHA={ABF:'5cedde6325a9098d4d3c57dcc0bb9a62e44dade06476deb6a41973111067b487',DDS:'a70a4a5a0782c808e164fbd0361d8c7ed4eddd6cf1e384a8408725dc03be80fc'}
ADDED={0xB6B1:(432,1720,17,19),0xD30D:(464,1720,18,19)}


def load(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))


def dump(p,d):
    p.parent.mkdir(parents=True,exist_ok=True)
    p.write_bytes((json.dumps(d,ensure_ascii=False,indent=2)+'\n').encode('utf8'))


def epoch_authority():
    name='title-menu-v12';seen=set();result={}
    while name:
        if name in seen or Path(name).name!=name:raise ValueError('Invalid epoch lineage')
        seen.add(name);d=load(ROOT/'localization/image_assets'/name/'ready.json')
        for row in d['files']:
            if row['filename'] in PACKS:result.setdefault(row['filename'],row['sha256'])
        name=d.get('previous_bundle',{}).get('name')
    if set(result)!=set(PACKS):raise ValueError('Incomplete v12 epoch')
    return result


def check_epoch(expected):
    for name,digest in expected.items():
        if sha_file(DISC/name)!=digest or sha_file(HDD/name)!=digest:
            raise ValueError('Installed v12 source differs: '+name)


def validate_font(old_abf,old_dds,new_abf,new_dds):
    if sha(old_abf)!=OLD_SHA[ABF] or sha(old_dds)!=OLD_SHA[DDS] or sha(new_abf)!=NEW_SHA[ABF] or sha(new_dds)!=NEW_SHA[DDS]:
        raise ValueError('Font payload differs from reviewed old/new authority')
    common,old_glyphs,pages=read_font(old_abf);newcommon,new_glyphs,newpages=read_font(new_abf)
    if len(old_glyphs)!=8175 or len(new_glyphs)!=8177 or old_glyphs!=new_glyphs[:8175] or pages!=newpages:
        raise ValueError('Old glyph records/page definitions changed')
    if {g['codepoint'] for g in new_glyphs}-{g['codepoint'] for g in old_glyphs}!=set(ADDED):
        raise ValueError('Unexpected added codepoints')
    for glyph in new_glyphs[8175:]:
        if tuple(glyph[k] for k in ('x','y','width','height'))!=ADDED[glyph['codepoint']]:
            raise ValueError('New glyph metrics changed')
        if tuple(glyph[k] for k in ('bearing_x','bearing_y','advance','page'))!=(1,4,19,0):
            raise ValueError('New glyph baseline changed')
    oldchunks=list(chunks(old_abf));newchunks=list(chunks(new_abf))
    if len(oldchunks)!=len(newchunks):raise ValueError('ABF chunk count changed')
    for old,new in zip(oldchunks,newchunks):
        if old[1:3]!=new[1:3]:raise ValueError('ABF chunk order changed')
        if old[1:3]==(b'BFNT',b'CBLK'):
            if new[3][:len(old[3])]!=old[3] or len(new[3])!=len(old[3])+64:raise ValueError('Old CBLK records changed')
        elif old[1:3]==(b'BFNT',b'HCMN'):
            if old[3][4:]!=new[3][4:] or old[4]!=new[4]:raise ValueError('ABF geometry changed')
        elif old[1:]!=new[1:]:raise ValueError('Other ABF chunks changed')
    if common['width']!=2048 or common['height']!=2048 or common['page_count']!=1 or newcommon!=dict(common,glyph_count=8177):
        raise ValueError('Font geometry changed')
    if old_dds[:128]!=new_dds[:128] or len(old_dds)!=len(new_dds) or old_dds[84:88]!=b'DXT5' or len(new_dds)!=128+2048*2048:
        raise ValueError('DDS header/geometry/length changed')
    allowed=set()
    for x in (428,460):
        for yy in range(1716//4,(1716+32)//4):
            for xx in range(x//4,(x+32)//4):allowed.add(yy*512+xx)
    changed=[];visible=0
    for index in range(2048*2048//16):
        at=128+16*index;old=old_dds[at:at+16];new=new_dds[at:at+16]
        occupied=any(decode_alpha(old));visible+=occupied
        if old!=new:
            if occupied or index not in allowed:raise ValueError('Old visible/outside-free-cell atlas block changed')
            changed.append(index)
    if len(changed)!=36:raise ValueError('Unexpected glyph block-change scope')
    alias=next(g for g in old_glyphs if g['codepoint']==0xB7);at=alias['record_offset']
    if old_abf[at:at+32]!=new_abf[at:at+32]:raise ValueError('Middle-dot alias record changed')
    return dict(status='PASS',source_glyphs=8175,candidate_glyphs=8177,added_codepoints=['U+B6B1','U+D30D'],
                old_CBLK_records_byte_identical=True,old_visible_atlas_blocks_byte_identical=True,
                old_visible_blocks=visible,changed_BC3_blocks=changed,
                all_atlas_bytes_outside_two_free_cells_identical=True,DDS_header_geometry_unchanged=True,
                ABF_other_chunks_unchanged=True,middle_dot_alias_record_identical=True)


def coverage(new_abf):
    codes={g['codepoint'] for g in read_font(new_abf)[1]};count=0;maximum=0;snapshots=[]
    for batch in (1,2,3):
        p=ROOT/f'localization/subtitles/battle-voice/translation-batches/batch-{batch}.ko.json';raw=p.read_bytes()
        snapshots.append(dict(path=str(p.relative_to(ROOT)),sha256=sha(raw)))
        for row in json.loads(raw)['entries']:
            text=row['translation_ko'];size=len(text.encode('utf8'));maximum=max(maximum,size);count+=1
            if any(ord(c) not in codes for c in text if c not in '\n\r\t'):raise ValueError('Proposed Korean caption missing glyph')
            if size>254:raise ValueError('Proposed Korean caption overflows 254 UTF8 bytes')
    if count!=413 or any(sha_file(ROOT/s['path'])!=s['sha256'] for s in snapshots):raise ValueError('Translation snapshots changed')
    return dict(status='PASS',proposed_entries=413,missing_codepoints=[],maximum_UTF8_bytes=maximum,
                native_limit_UTF8_bytes=254,translation_snapshots=snapshots,human_listening_verified=False)


def prepare():
    if load(ROOT/'localization/image_assets/active-ui.json')['bundle']!='title-menu-v12':raise ValueError('Require current v12 source')
    authority=load(B/'candidate/authority.json');entries=authority['entries']
    if len(entries)!=2 or {a['name'] for a in entries}!=set(OLD_SHA):raise ValueError('Require exact ABF/DDS pair')
    replacements={};extra=[]
    for a in entries:
        name=a['name'];p=Path(a['replacement']).resolve()
        if B.resolve() not in p.parents or a['path']!='data/font/'+name or a['source_sha256']!=OLD_SHA[name] or a['replacement_sha256']!=NEW_SHA[name]:
            raise ValueError('Unreviewed font authority path/hash')
        replacements[name]=p.read_bytes()
        if sha(replacements[name])!=NEW_SHA[name]:raise ValueError('Candidate changed')
        extra.append(dict(path=a['path'],type='ABF' if name==ABF else 'DDS',original_sha256=OLD_SHA[name],
                          replacement=str(p),replacement_sha256=NEW_SHA[name]))
    expected=epoch_authority();check_epoch(expected)
    _,prim=primary(DISC/'pack.idx')
    if any(p['name'] in OLD_SHA for p in prim):raise ValueError('Unexpected primary font route')
    _,archives=pidx(DISC/'pack.idx');matches=[];old_assets={};visited=0
    for ar in archives:
        header=region(DISC/ar['pack'],ar['offset'],20)
        if header[:4]!=b'FSTS':raise ValueError('Unknown secondary format')
        visited+=1;count,table,ns,nz=struct.unpack_from('<4I',header,4)
        bounded_range(ns,nz,ar['size']);bounded_range(table,count*16,ar['size'])
        names=region(DISC/ar['pack'],ar['offset']+ns,nz)
        if not any(n.encode() in names for n in OLD_SHA):continue
        blob=region(DISC/ar['pack'],ar['offset'],ar['size'])
        for row in fsts_entries(blob):
            name=Path(row['path']).name
            if name not in OLD_SHA:continue
            raw,_=decoded(blob,row)
            if ar['path']!='resident' or ar['pack']!='data.dat' or row['path']!='data/font/'+name or sha(raw)!=OLD_SHA[name]:
                raise ValueError('Unknown font route/variant')
            matches.append(dict(archive=ar,entry=row,source_sha256=sha(raw)));old_assets[name]=raw
    if len(matches)!=2 or set(old_assets)!=set(OLD_SHA):raise ValueError('Missing/duplicate resident-only font route')
    proof=validate_font(old_assets[ABF],old_assets[DDS],replacements[ABF],replacements[DDS]);fontcoverage=coverage(replacements[ABF])
    elfdoc=load(PREV/'ready.json')['elf'];elf=PREV/elfdoc['path']
    if sha_file(elf)!=elfdoc['sha256']:raise ValueError('Reviewed ELF changed')
    plan=dict(status='PREPARED_EXACT_V12',source_epoch=expected,authority_sha256=sha_file(B/'candidate/authority.json'),
              matches=matches,primary_count=0,FSTS_metadata_visited=visited,unknown_variants=0,
              font_proof=proof,font_coverage=fontcoverage,elf=dict(source=str(elf),sha256=elfdoc['sha256']),active_modified=False)
    dump(B/'source-plan.json',plan);dump(B/'nested-extra-map.json',dict(entries=extra));return plan


def apply(plan):
    out=B/'staged-final';artifacts=B/'nested-artifacts'
    if out.exists() or artifacts.exists() or (B/'EBOOT.catalog-pool.elf').exists():raise ValueError('Fresh staging outputs required')
    subprocess.run([sys.executable,str(ROOT/'tools/localization/stage_nested_ui.py'),'--source-dir',str(DISC),
                    '--extra-map',str(B/'nested-extra-map.json'),'--only-extra-map','--output',str(out),
                    '--artifacts',str(artifacts),'--apply'],check=True)
    nested=load(out/'nested-ui-manifest.json')
    if nested['unmatched'] or nested['candidates']!=2 or len(nested['archives'])!=1:raise ValueError('Unexpected nested scope')
    for name in PACKS:
        if (out/name).exists():continue
        shutil.copyfile(DISC/name,out/name)
        if (out/name).stat().st_nlink!=1 or sha_file(out/name)!=plan['source_epoch'][name]:raise ValueError('Preserved pack copy differs')
    shutil.copyfile(plan['elf']['source'],B/'EBOOT.catalog-pool.elf')
    if sha_file(B/'EBOOT.catalog-pool.elf')!=plan['elf']['sha256']:raise ValueError('ELF copy differs')
    check_epoch(plan['source_epoch'])
    dump(B/'ready-paths.json',dict(files={n:str(out/n) for n in PACKS},elf=str(B/'EBOOT.catalog-pool.elf'),
         source_epoch=plan['source_epoch'],active_modified=False))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--apply',action='store_true');args=parser.parse_args()
    plan=prepare()
    if args.apply:apply(plan)
    print(json.dumps(dict(matches=2,unknown_variants=0,applied=args.apply,active_modified=False)))
