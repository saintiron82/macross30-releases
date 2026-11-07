"""Prepare an isolated 8175->8177 BFNT/BC3 extension using the existing GDI pipeline.

No pack/index/ELF/launch pointer is staged, installed, or modified. Original
geometry and every old glyph record/visible atlas block are preserved.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

from audit_runtime_font import chunks, occupied_rows, place_cells, read_font, verify_placements
from build_korean_font import decode_alpha, sha
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_menu_ui_bundle import primary
from stage_ui_images import sha_file, bounded_range

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/image_assets/battle-voice-font-v13'
FONT_NAMES=('font_j24x24.abf','font_j24x24_0.dds')
SOURCE_SHA={
    FONT_NAMES[0]:'1d5ba9ee7c35a254a4a8a5d50e89e57d4c3025eeceaa3bd7a1d97ccb8628046c',
    FONT_NAMES[1]:'1f5f724d77b7ae8a9e50700ee6f86d884ac5113689f4ad9b7e1a4a42de34d5a1',
}
MISSING=(0xB6B1,0xD30D)


def load(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def dump(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes((json.dumps(value,ensure_ascii=False,indent=2)+'\n').encode('utf8'))


def read_sources():
    if load(ROOT/'localization/image_assets/active-ui.json')['bundle']!='title-menu-v12':
        raise ValueError('Require installed title-menu-v12 source epoch')
    ready=load(ROOT/'localization/image_assets/title-menu-v12/ready.json')
    expected={r['filename']:r['sha256'] for r in ready['files']}
    rows=[];primary_scans=[];visited={};assets={};fallbacks=[]
    for role,folder in (
        ('disc',ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'),
        ('hdd',ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'),
    ):
        if sha_file(folder/'pack.idx')!=expected['pack.idx']:
            raise ValueError('Current index differs from installed v12 authority')
        _,prim=primary(folder/'pack.idx')
        packs=sorted({r['pack'] for r in prim}|set(expected)|{'pack.idx'})
        for pack in packs:
            file=folder/pack
            if not file.exists() or region(file,0,4)!=b'PIDX':continue
            _,ps=primary(file);found=[r for r in ps if r['name'] in FONT_NAMES]
            if found:raise ValueError('Unexpected primary font route')
            primary_scans.append(dict(role=role,index=pack,records=len(ps),font_matches=0))
        _,archives=pidx(folder/'pack.idx');visited[role]=0
        for ar in archives:
            backing=folder/ar['pack']
            if not backing.exists():
                if role!='hdd' or ar['pack']!='sound.dat':
                    raise ValueError('Missing current backing pack '+str(backing))
                backing=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'/ar['pack']
                if ar['pack'] not in fallbacks:fallbacks.append(ar['pack'])
            header=region(backing,ar['offset'],20)
            if header[:4]!=b'FSTS':continue
            visited[role]+=1
            count,table,ns,nz=struct.unpack_from('<4I',header,4)
            bounded_range(ns,nz,ar['size']);bounded_range(table,count*16,ar['size'])
            names=region(backing,ar['offset']+ns,nz)
            if not any(n.encode() in names for n in FONT_NAMES):continue
            blob=region(backing,ar['offset'],ar['size'])
            for entry in fsts_entries(blob):
                name=Path(entry['path']).name
                if name not in FONT_NAMES:continue
                if backing.parent!=folder:
                    raise ValueError('Selected font must exist in its actual install, not fallback')
                raw,_=decoded(blob,entry)
                if ar['path']!='resident' or entry['path']!='data/font/'+name or sha(raw)!=SOURCE_SHA[name]:
                    raise ValueError('Unknown font copy/variant')
                if name in assets and assets[name]!=raw:
                    raise ValueError('Disc/HDD font payload differs')
                assets[name]=raw
                rows.append(dict(role=role,archive=ar,entry=entry,raw_sha256=sha(raw)))
        if sha_file(folder/'pack.idx')!=expected['pack.idx']:
            raise ValueError('Index changed during read-only scan')
    if len(rows)!=4 or any(sum(r['role']==role and Path(r['entry']['path']).name==name for r in rows)!=1
                           for role in ('disc','hdd') for name in FONT_NAMES):
        raise ValueError('Expected resident-only font ABF/DDS, one copy each per install')
    return assets,dict(status='PASS',active_bundle='title-menu-v12',ready_version=ready['version'],
                       expected_pack_hashes_from_ready=expected,pack_hash_scope='Current index SHA and actual font raw SHA checked; multi-GB backing hashes are authority metadata only',
                       primary_scans=primary_scans,FSTS_metadata_visited=visited,nested_copies=rows,
                       hdd_missing_nonfont_archive_backings_read_from_disc=fallbacks,
                       disc_hdd_font_payloads_identical=True,active_modified=False)


def prepare():
    if (B/'candidate').exists() or (B/'source').exists():
        raise ValueError('Fresh font candidate/source paths required')
    assets,provenance=read_sources()
    abf=assets[FONT_NAMES[0]];dds=assets[FONT_NAMES[1]]
    common,glyphs,pages=read_font(abf)
    old_codes={g['codepoint'] for g in glyphs}
    if len(glyphs)!=8175 or len(old_codes)!=8175 or any(c in old_codes for c in MISSING):
        raise ValueError('Unexpected 8175-glyph source repertoire')
    if common['page_count']!=1 or dds[84:88]!=b'DXT5' or len(dds)!=128+common['width']*common['height']:
        raise ValueError('Source atlas format/geometry changed')
    # Existing glyph metrics plus actual nontransparent blocks are occupied.
    # This protects orphan/unused visible atlas pixels too, not merely ABF refs.
    rows=occupied_rows(common,glyphs,4);width=common['width'];height=common['height'];visible=set()
    for index in range(width*height//16):
        if any(decode_alpha(dds[128+index*16:144+index*16])):
            visible.add(index);x=(index%(width//4))*4;y=(index//(width//4))*4
            for yy in range(y,y+4):rows[0][yy]|=15<<x
    placements=place_cells(rows,width,height,list(MISSING),24,4,4)
    fits=len(placements)==len(MISSING)
    audit=dict(source=str(B/'source'/FONT_NAMES[0]),source_sha256=sha(abf),source_dds_sha256=sha(dds),
               font=common,page_names=pages,original_glyphs=len(glyphs),required_missing_hangul=2,
               glyph_size=24,padding_each_side=4,compression_block_size=4,placements=placements,
               fits_preserving_original_rectangles=fits,actual_visible_BC3_blocks_protected=len(visible),
               geometry_change_allowed=False)
    dump(B/'font-placement-audit.json',audit);dump(B/'source-provenance.json',provenance)
    if not fits:raise ValueError('Two glyph cells do not fit; atlas geometry unchanged, stop for review')
    verify_placements(occupied_rows(common,glyphs,4),placements)
    (B/'source').mkdir(parents=True)
    for name,raw in assets.items():(B/'source'/name).write_bytes(raw)
    prior=load(ROOT/'localization/image_assets/data-ui-v6/font-incremental/manifest.json')
    font=ROOT/prior['source_font']['path']
    if sha_file(font)!=prior['source_font']['sha256']:
        raise ValueError('Previously verified Korean source font changed')
    subprocess.run([sys.executable,str(ROOT/'tools/localization/build_korean_font.py'),
                    '--audit',str(B/'font-placement-audit.json'),'--abf',str(B/'source'/FONT_NAMES[0]),
                    '--dds',str(B/'source'/FONT_NAMES[1]),'--font',str(font),
                    '--output',str(B/'candidate')],check=True,timeout=120)
    newabf=(B/'candidate'/FONT_NAMES[0]).read_bytes();newdds=(B/'candidate'/FONT_NAMES[1]).read_bytes()
    newcommon,newglyphs,newpages=read_font(newabf)
    if newcommon['glyph_count']!=8177 or newglyphs[:8175]!=glyphs or newpages!=pages:
        raise ValueError('Old font metrics/positions changed')
    if newdds[:128]!=dds[:128] or len(newdds)!=len(dds):
        raise ValueError('Atlas header/dimensions/length changed')
    changed=[];reserved=set()
    for p in placements:
        cell=p['reserved']
        for y in range(cell['y']//4,(cell['y']+cell['height'])//4):
            for x in range(cell['x']//4,(cell['x']+cell['width'])//4):reserved.add(y*(width//4)+x)
    for index in range(width*height//16):
        start=128+index*16
        if newdds[start:start+16]!=dds[start:start+16]:
            if index not in reserved or index in visible:raise ValueError('Old visible/outside-reservation atlas block changed')
            changed.append(index)
    if not changed:raise ValueError('Empty glyph atlas extension')
    alias=next(g for g in glyphs if g['codepoint']==0xB7)
    aliasnew=next(g for g in newglyphs if g['codepoint']==0xB7)
    if alias!=aliasnew or abf[alias['record_offset']:alias['record_offset']+32]!=newabf[aliasnew['record_offset']:aliasnew['record_offset']+32]:
        raise ValueError('Middle-dot alias changed')
    old_chunks=list(chunks(abf));new_chunks=list(chunks(newabf))
    if len(old_chunks)!=len(new_chunks):raise ValueError('ABF chunk count changed')
    for old,new in zip(old_chunks,new_chunks):
        if old[1:3]!=new[1:3]:raise ValueError('ABF chunk order changed')
        if old[1:3]==(b'BFNT',b'CBLK'):
            if new[3][:len(old[3])]!=old[3]:raise ValueError('Old CBLK bytes changed')
        elif old[1:3]==(b'BFNT',b'HCMN'):
            if new[3][4:]!=old[3][4:] or new[4]!=old[4]:raise ValueError('HCMN geometry changed')
        elif old[1:]!=new[1:]:raise ValueError('Other ABF chunks changed')
    coverage={g['codepoint'] for g in newglyphs};proposed=[];missing={};snapshots=[]
    for batch in (1,2,3):
        file=ROOT/f'localization/subtitles/battle-voice/translation-batches/batch-{batch}.ko.json'
        raw=file.read_bytes();snapshots.append(dict(path=str(file.relative_to(ROOT)),sha256=sha(raw)))
        for index,entry in enumerate(json.loads(raw)['entries']):
            absent=sorted({ord(c) for c in entry['translation_ko'] if c not in '\n\r\t' and ord(c) not in coverage})
            if absent:missing[f'{batch}:{index}']=absent
            proposed.append((batch,index))
    if len(proposed)!=413 or missing:raise ValueError('Proposed Korean captions still lack glyphs')
    if any(sha_file(ROOT/s['path'])!=s['sha256'] for s in snapshots):raise ValueError('Translation outputs changed during coverage audit')
    # Verify the current font raw payloads remain identical after the raster build.
    after,after_proof=read_sources()
    if after!=assets:raise ValueError('Active source font changed during preparation')
    verification=dict(status='PASS',original_glyphs=8175,candidate_glyphs=8177,
                      added=[dict(codepoint=f'U+{c:04X}',character=chr(c)) for c in MISSING],
                      old_CBLK_records_byte_identical=True,old_visible_atlas_blocks_byte_identical=True,
                      all_atlas_blocks_outside_two_free_reservations_byte_identical=True,
                      preserved_visible_blocks=len(visible),changed_BC3_blocks=changed,
                      DDS_header_dimensions_length_unchanged=True,ABF_pages_other_chunks_preserved=True,
                      middle_dot_alias_record_byte_identical=True,middle_dot_alias=alias,
                      proposed_translation_entries_checked=413,translation_snapshots=snapshots,
                      missing_candidate_font_codepoints=[],font_coverage='PASS all 413 proposed KO strings',
                      active_modified=False,staged=False,installed=False,game_rendering_verified=False)
    dump(B/'candidate-verification.json',verification)
    authority=dict(status='OFFLINE_FONT_CANDIDATE',entries=[dict(name=name,path='data/font/'+name,
                    type='ABF' if name.endswith('.abf') else 'DDS',source=str(B/'source'/name),
                    source_sha256=SOURCE_SHA[name],replacement=str(B/'candidate'/name),
                    replacement_sha256=sha((B/'candidate'/name).read_bytes())) for name in FONT_NAMES],
                   source_epoch=provenance,placement_audit=str(B/'font-placement-audit.json'),
                   verification=str(B/'candidate-verification.json'),old_glyph_count=8175,new_glyph_count=8177,
                   active_modified=False,staged=False,installed=False)
    dump(B/'candidate/authority.json',authority)
    print(json.dumps(dict(status='PASS',candidate_glyphs=8177,added=2,changed_blocks=len(changed),
                         geometry_unchanged=True,font_coverage='PASS 413',active_modified=False),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.parse_args();prepare()
