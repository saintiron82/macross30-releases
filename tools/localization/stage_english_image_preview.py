"""Stage English DDS into every exact original primary/secondary resource route."""
import json
import os
from pathlib import Path
import shutil
import struct
import argparse
from datetime import datetime, timezone
from english_pack import ROOT, digest, write,preview_version
from build_english_ui_images import BASE, read
from stage_gop_primary_mirrors import pidx, read_payload
from stage_nested_ui import fsts_entries, decoded, rebuild, region


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version',type=preview_version,default='v1')
    parser.add_argument('--no-activate',action='store_true')
    args=parser.parse_args();version=args.version;epoch=int(version[1:])
    source=ROOT/'build/english-preview'
    target=ROOT/('build/english-preview-images-'+version)
    pack_rel=Path('PS3_GAME/USRDIR/DATA/PACK')
    pack_dir=source/pack_rel
    if target.exists():
        raise FileExistsError(target)
    mappings={}
    def add(name,sha,path):
        payload=Path(path).read_bytes()
        key=(name,sha)
        if key in mappings and mappings[key]!=payload:
            raise ValueError('Conflicting English image authority')
        mappings[key]=payload
    for name in ('title_logo02.dds','title_text.dds'):
        original=(ROOT/'analysis/localization/title-menu-v11-audit/nested'/name).read_bytes()
        payload=ROOT/'localization/en-US/images/title-v1'/name
        if original[:128]!=payload.read_bytes()[:128]:
            raise ValueError('Title DDS header changed')
        add(name,digest(original),payload)
    for name in ('opt_text00','opt_text01','opt_logo00','opt_brighttext'):
        original=ROOT/'analysis/assets/data'/(name+'.dds')
        add(name+'.dds',digest(original.read_bytes()),BASE/'replacements'/(name+'.dds'))
    for name in ('hangar_menu01.dds','hangar_menu02.dds','hangar_text01.dds','hng_custom_icon00.dds','hng_develop_bar.dds','hng_selectguide.dds','hng_selectguide_weapon.dds'):
        original=ROOT/'native/analysis/develop-ui-localization'/name
        add(name,digest(original.read_bytes()),BASE/'replacements'/name)
    for folder in ('misc-replacements','region-replacements','hud-replacements','dialogue-variant-replacements','remaining-replacements'):
        for row in read(BASE/folder/'misc-replacements-manifest.json')['assets']:
            replacement=row['replacement']
            if row['name']=='ope60_title.dds' and epoch>=6:
                banner_folder='combat-banner-v3' if epoch>=12 else 'combat-banner-v2'
                replacement=read(ROOT/('localization/en-US/images/'+banner_folder+'/manifest.json'))['replacement']
            add(row['name'],row['source_sha256'],replacement)
    for row in read(BASE/'gallery-title-replacements/manifest.json')['files']:
        add(row['name'],row['source_sha256'],BASE/'gallery-title-replacements'/row['name'])
    for row in read(BASE/'quest-replacements/quest-title-batch-manifest.json')['textures']:
        add(row['source_filename'],row['original_sha256'],BASE/'quest-replacements'/row['source_filename'])
    for row in read(BASE/'place-bcf-replacements/manifest.json')['files']:
        add(row['filename'],row['source_sha256'],BASE/'place-bcf-replacements'/row['filename'])
    loading=ROOT/'localization/en-US/images/loading-names-v1/manifest.json'
    if epoch>=5:
        for row in read(loading)['assets']:
            add(row['name'],row['source_sha256'],row['replacement'])
    revised=set()
    if epoch>=12:
        banner=read(ROOT/'localization/en-US/images/combat-banner-v3/manifest.json')
        revised.add((banner['name'],banner['source_sha256']))
    if epoch>=11:
        for row in read(ROOT/'localization/en-US/images/residual-v11/manifest.json')['assets']:
            revised.add((row['name'],row['source_sha256']))
    if epoch>=9:
        folder='extra-ui-v10' if epoch>=10 else 'extra-ui-v1'
        for row in read(ROOT/('localization/en-US/images/'+folder+'/manifest.json'))['assets']:
            if digest(Path(row['replacement']).read_bytes())!=row['replacement_sha256']:raise ValueError('Additional English UI candidate changed')
            add(row['name'],row['source_sha256'],row['replacement'])
            if epoch>=10:revised.add((row['name'],row['source_sha256']))
    if epoch>=10:
        for row in read(ROOT/'localization/en-US/images/discovery-v10/manifest.json')['assets']:
            if digest(Path(row['replacement']).read_bytes())!=row['replacement_sha256']:raise ValueError('Discovery image changed')
            add(row['name'],row['source_sha256'],row['replacement'])
            revised.add((row['name'],row['source_sha256']))
    registry=ROOT/'tools/localization/catalogs/image_registry.json'
    if registry.exists():
        for asset in read(registry)['assets']:
            if asset['category']!='ui_image':continue
            if asset.get('scope')=='additional_original' and not asset['english_status'].startswith('installed'):continue
            candidate=ROOT/asset['source_files']['en-US']
            if digest(candidate.read_bytes())!=asset['english_sha256']:
                raise ValueError('Registered English image checksum changed: '+asset['id'])
            if not asset['english_status'].startswith('installed'):
                raise ValueError('Registered English image is not ready: '+asset['id'])
            if (asset['name'],asset['original_sha256']) not in revised:
                add(asset['name'],asset['original_sha256'],candidate)
    if epoch>=11:
        for row in read(ROOT/'localization/en-US/images/residual-v11/manifest.json')['assets']:
            key=(row['name'],row['source_sha256']);payload=Path(row['replacement']).read_bytes()
            if key not in mappings or digest(payload)!=row['replacement_sha256'] or payload[:128]!=mappings[key][:128] or len(payload)!=len(mappings[key]):raise ValueError('Residual image override identity changed')
            mappings[key]=payload
    if epoch>=6:
        banner_folder='combat-banner-v3' if epoch>=12 else 'combat-banner-v2'
        banner=read(ROOT/('localization/en-US/images/'+banner_folder+'/manifest.json'))
        key=(banner['name'],banner['source_sha256'])
        if key not in mappings:raise ValueError('Unknown banner replacement authority')
        candidate=Path(banner['replacement']).read_bytes()
        if epoch>=12 and digest(candidate)!=banner['replacement_sha256']:
            raise ValueError('Rebuilt combat banner checksum changed')
        original=(ROOT/'native/analysis/combat-caution-ui/ope60_title.dds').read_bytes()
        if digest(original)!=key[1] or original[:128]!=candidate[:128] or len(original)!=len(candidate):
            raise ValueError('Banner header/source mismatch')
        changed=[i for i,(a,b) in enumerate(zip(original,candidate)) if a!=b]
        if not changed or any(i<128 or i>=128+512*384*4 or (i-128)%4!=3 for i in changed):
            raise ValueError('Banner changed protected RGB/header/blank rows')
        mappings[key]=candidate
    index=pack_dir/'PACK.IDX'
    _,primary,archives=pidx(index)
    pack_paths={p.name.lower():p for p in pack_dir.iterdir() if p.is_file()}
    names={k[0] for k in mappings}
    primary_changes=[];secondary_changes=[];seen=set();unknown=[]
    for row in primary:
        if row['name'] not in names:
            continue
        path=pack_paths[Path(row['pack']).name.lower()]
        payload,_=read_payload(path,row)
        key=(row['name'],digest(payload))
        if key not in mappings:
            unknown.append(dict(route='primary',name=row['name'],sha256=key[1]));continue
        _,embedded,_=pidx(path)
        aliases=[r for r in embedded if all(r[k]==row[k] for k in ('name','pack','offset','size','compressed_size'))]
        if len(aliases)!=1:
            raise ValueError('Embedded primary alias mismatch')
        primary_changes.append((row,aliases[0],key))
        seen.add(key)
    for archive in archives:
        path=pack_paths[Path(archive['pack']).name.lower()]
        head=region(path,archive['offset'],20)
        if head[:4]!=b'FSTS':continue
        count,table,ns,nz=struct.unpack_from('<4I',head,4)
        table_names=region(path,archive['offset']+ns,nz)
        if not any(n.encode() in table_names for n in names):continue
        blob=region(path,archive['offset'],archive['size'])
        patches={}
        for entry in fsts_entries(blob):
            name=Path(entry['path']).name
            if name not in names:continue
            payload,_=decoded(blob,entry)
            key=(name,digest(payload))
            if key not in mappings:
                unknown.append(dict(route='nested',name=entry['path'],sha256=key[1]));continue
            patches[entry['index']]=(key[1],mappings[key]);seen.add(key)
        if not patches:continue
        rebuilt,proof=rebuild(blob,patches)
        _,_,embedded=pidx(path)
        aliases=[r for r in embedded if all(r[k]==archive[k] for k in ('name','pack','offset','size'))]
        if len(aliases)!=1:raise ValueError('Embedded archive alias mismatch')
        secondary_changes.append((archive,aliases[0],rebuilt,proof))
    if seen!=set(mappings):
        raise ValueError('Missing original image routes: '+str(set(mappings)-seen))
    if unknown:
        raise ValueError('Unknown image variants: '+str(unknown))
    touched={Path(r[0]['pack']).name.lower() for r in primary_changes+secondary_changes}|{'pack.idx'}
    for path in sorted((source/'PS3_GAME').rglob('*')):
        if not path.is_file():continue
        relative=path.relative_to(source);output=target/relative
        output.parent.mkdir(parents=True,exist_ok=True)
        if path.parent==pack_dir and path.name.lower() in touched:
            shutil.copyfile(path,output)
        else:os.link(path,output)
    shutil.copyfile(source/'PS3_DISC.SFB',target/'PS3_DISC.SFB')
    outputs={p.name.lower():p for p in (target/pack_rel).iterdir()}
    checks=[]
    def append(path,payload):
        with path.open('ab') as stream:
            stream.write(b'\0'*(-stream.tell()%2048));offset=stream.tell();stream.write(payload)
        return offset
    def update(path,offset,values):
        with path.open('r+b') as stream:
            stream.seek(offset);stream.write(struct.pack('<'+'I'*len(values),*values))
    for master,embedded,key in primary_changes:
        path=outputs[Path(master['pack']).name.lower()];payload=mappings[key]
        offset=append(path,payload)
        for destination,row in ((outputs['pack.idx'],master),(path,embedded)):
            update(destination,row['record']+12,(offset,len(payload),0))
        checks.append(dict(name=key[0],sha256=digest(payload),route='primary',pack=path.name,record=master['record']))
    for master,embedded,payload,proof in secondary_changes:
        path=outputs[Path(master['pack']).name.lower()];offset=append(path,payload)
        for destination,row in ((outputs['pack.idx'],master),(path,embedded)):
            update(destination,row['record']+8,(offset,len(payload)))
        checks.append(dict(name=master['name'],sha256=digest(payload),route='archive',pack=path.name,record=master['record'],entries=len(proof)))
    _,final_primary,final_archives=pidx(outputs['pack.idx'])
    for check in checks:
        row=next(r for r in (final_primary if check['route']=='primary' else final_archives) if r['record']==check['record'])
        if digest(read_payload(outputs[check['pack'].lower()],row)[0])!=check['sha256']:
            raise ValueError('Installed route checksum mismatch')
    state=ROOT/('native/state-en-preview-images-'+version)
    installed=state/'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
    installed.mkdir(parents=True)
    for path in (target/pack_rel).iterdir():os.link(path,installed/path.name.lower())
    (state/'hdd1').mkdir()
    run=ROOT/'native/analysis'/('english-images-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    run.mkdir(parents=True)
    if (ROOT/'native/play/gamedata').exists():shutil.copytree(ROOT/'native/play/gamedata',run/'gamedata')
    old=read(source/'preview.json');command=old['command']
    command[command.index('--vfs')+1]=str(target)
    command[command.index('--run-directory')+1]=str(run)
    for i,text in enumerate(command):
        if text.startswith('PS3_HDD0_ROOT='):command[i]='PS3_HDD0_ROOT='+str(state/'hdd0')
        if text.startswith('PS3_HDD1_ROOT='):command[i]='PS3_HDD1_ROOT='+str(state/'hdd1')
        if text.startswith('PS3_GAMEDATA_ROOT='):command[i]='PS3_GAMEDATA_ROOT='+str(run/'gamedata')
    write(target/'preview.json',dict(status='PARTIAL_ENGLISH_WITH_IMAGES',command=command,run_directory=str(run),
                                    translated_image_variants=len(mappings),primary_routes=len(primary_changes),
                                    nested_archives=len(secondary_changes),checks=checks,unknown_variants=unknown,
                                    live_game_verified=False))
    if not args.no_activate:write(ROOT/'localization/en-US/preview-active.json',dict(manifest=str(target/'preview.json')))
    print(json.dumps(dict(english_image_variants=len(mappings),primary_routes=len(primary_changes),nested_archives=len(secondary_changes),preview=str(target))))


if __name__=='__main__':main()
