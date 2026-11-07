"""Prepare a separate KO menu bundle; installer defers if the game is open."""
import json
import shutil
import argparse
import struct
from pathlib import Path
from english_pack import ROOT,digest,write
from stage_menu_ui_bundle import primary,make_plan,audit_nested
from stage_ui_images import apply,sha_file
from stage_gop_primary_mirrors import read_payload,pidx
from stage_nested_ui import region,fsts_entries,decoded,rebuild


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--with-itembox-tabs',action='store_true')
    parser.add_argument('--with-quest-badges',action='store_true')
    parser.add_argument('--with-viewer-guide',action='store_true');args=parser.parse_args()
    # Quest revision includes all previously pending menu changes.
    if args.with_viewer_guide:args.with_quest_badges=True
    if args.with_quest_badges:args.with_itembox_tabs=True
    name='viewer-guide-menu-v19' if args.with_viewer_guide else 'quest-badges-menu-v18' if args.with_quest_badges else 'itembox-menu-v17' if args.with_itembox_tabs else 'custom-sound-menu-v16'
    b=ROOT/'localization/image_assets'/name;b.mkdir(exist_ok=True)
    active_path=ROOT/'localization/image_assets/active-ui.json';active=json.loads(active_path.read_text(encoding='utf8'))
    assert active['bundle'] in ('battle-voice-font-v13','custom-sound-menu-v16','itembox-menu-v17','quest-badges-menu-v18')
    prior=ROOT/'localization/image_assets'/active['bundle'];ready=json.loads((prior/'ready.json').read_text(encoding='utf8'))
    source=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
    hdd=ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
    authorities=json.loads((ROOT/'localization/custom-sound/menus.json').read_text(encoding='utf8'))['assets']
    if args.with_itembox_tabs:
        authorities.append(json.loads((ROOT/'localization/itembox-tabs/manifest.json').read_text(encoding='utf8')))
    if args.with_quest_badges:
        authorities.append(json.loads((ROOT/'localization/quest-badges/manifest.json').read_text(encoding='utf8')))
    if args.with_viewer_guide:
        authorities.append(json.loads((ROOT/'localization/viewer-guide/manifest.json').read_text(encoding='utf8'))['ko-KR'])
    # A previous menu bundle may already have been installed. Reuse its exact
    # replacement and patch only the remaining original inputs.
    _,current_rows=primary(source/'pack.idx');pending=[]
    for a in authorities:
        row=next((r for r in current_rows if r['name']==a['name']),None)
        if row is None:pending.append(a);continue # nested-only, checked by audit_nested
        raw,_=read_payload(source/row['pack'],row)
        if digest(raw)==a['replacement_sha256']:continue
        assert digest(raw)==a['source_sha256'];pending.append(a)
    authorities=pending
    nested=audit_nested(source,dict(authorities=authorities,nested_entries=[]),discover_reviewed=True)
    _,rows=primary(source/'pack.idx');selected=[r for r in rows if r['name'] in {a['name'] for a in authorities}]
    assert len(selected)<=len(authorities) and selected
    index=source/'pack.idx';staged={}
    for i,name in enumerate(sorted({r['pack'] for r in selected})):
        assert sha_file(source/name)==sha_file(hdd/name)
        plan=make_plan(source/name,index,authorities,sha_file(source/name),sha_file(index))
        out=b/f'stage-{i}';apply(plan,out);staged[name]=out/name;index=out/'pack.idx'
    staged['pack.idx']=index
    # The item-box atlas is also inside Resident/menu FSTS archives. Rebuild
    # every observed copy and update both PIDX mirrors, preserving old payloads.
    _,_,archives=pidx(source/'pack.idx');names={a['name']:a for a in authorities};nested_checks=[]
    for archive in archives:
        if not any(m['archive_offset']==archive['offset'] and Path(m['pack']).name.lower()==Path(archive['pack']).name.lower() for m in nested['matches']):continue
        name=Path(archive['pack']).name.lower();original_pack=source/name
        blob=region(original_pack,archive['offset'],archive['size']);patches={}
        for entry in fsts_entries(blob):
            a=names.get(Path(entry['path']).name)
            if a is None:continue
            payload,_=decoded(blob,entry);assert digest(payload)==a['source_sha256']
            patches[entry['index']]=(a['source_sha256'],Path(a['replacement']).read_bytes())
        rebuilt,proof=rebuild(blob,patches)
        if name not in staged:
            assert sha_file(original_pack)==sha_file(hdd/name)
            destination=b/name;shutil.copyfile(original_pack,destination);staged[name]=destination
        destination=staged[name];_,_,embedded=pidx(destination)
        aliases=[r for r in embedded if all(r[k]==archive[k] for k in ('name','pack','offset','size'))]
        assert len(aliases)==1
        with destination.open('ab') as stream:
            stream.write(b'\0'*(-stream.tell()%2048));offset=stream.tell();stream.write(rebuilt)
        for path,row in [(index,archive),(destination,aliases[0])]:
            with path.open('r+b') as stream:stream.seek(row['record']+8);stream.write(struct.pack('<2I',offset,len(rebuilt)))
        nested_checks.append(dict(record=archive['record'],pack=name,sha256=digest(rebuilt),entries=proof))
    _,_,final_archives=pidx(index)
    for check in nested_checks:
        row=next(r for r in final_archives if r['record']==check['record'])
        assert digest(region(staged[check['pack']],row['offset'],row['size']))==check['sha256']
    for a in authorities:
        _,actual=primary(index);row=next((r for r in actual if r['name']==a['name']),None)
        if row is None:
            assert any(Path(m['path']).name==a['name'] for m in nested['matches']);continue
        raw,_=read_payload(staged[row['pack']],row);assert digest(raw)==a['replacement_sha256']
    selected_elf=active.get('elf_override',ready['elf'])
    elf=prior/selected_elf['path'];assert sha_file(elf)==selected_elf['sha256']
    shutil.copyfile(elf,b/elf.name)
    files=[dict(filename=name,candidate=str(path.relative_to(b)),source_sha256=sha_file(source/name),sha256=sha_file(path)) for name,path in staged.items()]
    doc=dict(files=files,elf=dict(path=elf.name,sha256=sha_file(elf)),manifests=[],status='prepared menu-only bundle; install on next launch',in_game_verified=False)
    doc['version']=digest(json.dumps(doc,sort_keys=True).encode());write(b/'ready.json',doc)
    write(b/'previous-active.json',active)
    write(b/'review.json',dict(status='PASS',primary_routes=len(selected),nested_routes=len(nested['matches']),nested_readback=nested_checks,music_ELF_unchanged=True,game_render_verified=False))
    # Choosing the next-launch bundle is safe while the existing game continues;
    # the standard installer still enforces the runner lease before any install.
    write(active_path,dict(bundle=b.name))
    print(json.dumps(dict(bundle=b.name,images=len(selected),installed=False,next_launch_install=True)))


if __name__=='__main__':main()
