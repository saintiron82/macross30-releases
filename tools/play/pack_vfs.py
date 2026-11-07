"""Materialize proved GOP overlays into an isolated English-image VFS cache."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys


def _cache_files(cache):
    from pack_registry import safe_path
    from build_russian_pack import packfile
    pack=safe_path(cache,'vfs/PS3_GAME/USRDIR/DATA/PACK')
    paths=[packfile(pack,name) for name in ('DATA.DAT','PACK.IDX')]
    paths += [safe_path(cache,'state/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack/'+name)
              for name in ('data.dat','pack.idx')]
    return {safe_path(cache,path.relative_to(cache).as_posix()) for path in paths}


def _verify_cached(cache,key):
    from pack_registry import safe_path,digest
    proof=json.loads(safe_path(cache,'cache.json').read_text(encoding='utf8'))
    if proof.get('manifest_sha256')!=key or not isinstance(proof.get('files'),list):
        raise ValueError('Materialized pack cache manifest changed')
    expected=_cache_files(cache);proved=set()
    for row in proof['files']:
        if not isinstance(row,dict) or set(row)!={'path','sha256'}:raise ValueError('Invalid cache proof')
        path=safe_path(cache,row['path'])
        if path not in expected or path in proved or not path.is_file() or digest(path)!=row['sha256']:
            raise ValueError('Materialized pack cache changed')
        proved.add(path)
    if proved!=expected:raise ValueError('Materialized pack cache proofs missing')


def materialize_pack_vfs(root,doc,folder,base_profile,overlay):
    root,folder,overlay=map(Path,(root,folder,overlay))
    if doc.get('base_locale')!='en-US' or base_profile.get('locale')!='en-US':
        raise ValueError('GOP overlay v1 requires a proved English base')
    sys.path.insert(0,str(root/'tools/localization'))
    from build_russian_pack import patch_pack,private_file,packfile
    from stage_gop_primary_mirrors import pidx,read_payload,fsts_entries
    from gop_strings import parse
    from audit_runtime_font import read_font
    key=hashlib.sha256((folder/'pack.json').read_bytes()).hexdigest()
    from pack_registry import safe_path,digest,create_inherited_stage
    cache=safe_path(root,'native/installed-language-cache/'+doc['locale']+'/'+key)
    marker=safe_path(cache,'cache.json')
    if marker.exists():
        _verify_cached(cache,key)
        return cache/'vfs',cache/'state'
    cache.parent.mkdir(parents=True,exist_ok=True)
    stage=create_inherited_stage(cache.parent,'.build-')
    try:
        src=base_profile['vfs']/'PS3_GAME/USRDIR/DATA/PACK'
        _,rows,archives=pidx(src/'PACK.IDX')
        replacements={};expected={}
        for p in sorted(overlay.iterdir()):
            if p.name in ('font_j24x24.abf','font_j24x24_0.dds'):continue
            if not p.is_file() or p.suffix!='.gop':raise ValueError('Unsupported GOP overlay payload: '+p.name)
            matches=[r for r in rows if r['name']==p.name]
            if not matches:raise ValueError('Unknown GOP resource: '+p.name)
            before=read_payload(packfile(src,matches[0]['pack']),matches[0])[0]
            after=p.read_bytes();a,b=parse(before),parse(after)
            if len(a)!=len(b) or any(x['metadata']!=y['metadata'] or x['metadata'] and x['text']!=y['text'] for x,y in zip(a,b)):
                raise ValueError('GOP identity/metadata changed: '+p.name)
            # Builder accepts only a reflow of the same string section.
            from build_russian_pack import rebuild_gop
            if rebuild_gop(before,{y['index']:y['text'] for y in b if not y['metadata']})!=after:
                raise ValueError('Non-string GOP content changed: '+p.name)
            replacements[p.name]=after;expected[p.name]=hashlib.sha256(before).hexdigest()
        if not replacements and not (overlay/'font_j24x24.abf').is_file():raise ValueError('GOP overlay contains neither text nor font resources')
        resident=next(r for r in archives if r['name']=='resident')
        entries=fsts_entries(read_payload(packfile(src,resident['pack']),resident)[0])
        original=next(e['payload'] for e in entries if e['name']=='data/font/font_j24x24.abf')
        font=(overlay/'font_j24x24.abf').read_bytes() if (overlay/'font_j24x24.abf').exists() else original
        font_atlases={}
        if (overlay/'font_j24x24_0.dds').is_file():
            from pack_font import validate_extension
            resident=next(r for r in archives if r['name']=='resident')
            entries=fsts_entries(read_payload(packfile(src,resident['pack']),resident)[0])
            original_abf=next(e['payload'] for e in entries if e['name']=='data/font/font_j24x24.abf')
            original_dds=next(e['payload'] for e in entries if e['name']=='data/font/font_j24x24_0.dds')
            extended_dds=(overlay/'font_j24x24_0.dds').read_bytes()
            validate_extension(original_abf,original_dds,font,extended_dds)
            font_atlases['data/font/font_j24x24_0.dds']=extended_dds
        else:
            old_common,old_rows,old_atlas=read_font(original);common,new_rows,atlas=read_font(font)
            if old_common!=common or old_atlas!=atlas or len(old_rows)!=len(new_rows):raise ValueError('Added font glyphs require a verified font atlas')
            for a,b in zip(old_rows,new_rows):
                if any(a[k]!=b[k] for k in a if k not in ('advance','bearing_x')):raise ValueError('Font glyph mapping changed')
        shutil.copytree(base_profile['vfs'],stage/'vfs',copy_function=os.link)
        dest=stage/'vfs/PS3_GAME/USRDIR/DATA/PACK'
        for name in ('DATA.DAT','PACK.IDX'):private_file(packfile(dest,name))
        proof=patch_pack(dest,replacements,expected,font_payload=font,font_atlases=font_atlases,proof_directory=stage/'audit')
        game_root=Path(base_profile['state'])/'hdd0/game'
        def copy_state(source,destination):
            # Original game archives are immutable; saves/settings are private.
            if Path(source).is_relative_to(game_root):
                os.link(source,destination)
            else:
                shutil.copyfile(source,destination)
            return destination
        shutil.copytree(base_profile['state'],stage/'state',copy_function=copy_state)
        hdd=stage/'state/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
        hdd.mkdir(parents=True,exist_ok=True)
        for name in ('DATA.DAT','PACK.IDX'):
            target=hdd/name.lower()
            if target.exists():private_file(target)
            shutil.copyfile(packfile(dest,name),target)
        files=[dict(path=p.relative_to(stage).as_posix(),sha256=digest(p)) for p in sorted(_cache_files(stage))]
        (stage/'cache.json').write_text(json.dumps(dict(manifest_sha256=key,resource_readback=proof,files=files),indent=2),encoding='utf8')
        if cache.exists():raise ValueError('Concurrent cache build detected; retry after other build')
        stage.rename(cache)
        return cache/'vfs',cache/'state'
    finally:
        if stage.exists():shutil.rmtree(stage)
