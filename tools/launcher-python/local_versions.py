"""Reuse verified runtime caches and park completed translations on the same volume."""
import json
import os
from pathlib import Path
import re
import uuid
import hashlib

from release_selection import safe, digest, VERSION


def read(path):
    return json.loads(path.read_text('utf-8-sig'))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending=path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    pending.write_text(json.dumps(value,ensure_ascii=False),encoding='utf8')
    os.replace(pending,path)


def profiles(root):
    cache=safe(root,'.local-build/version-switch-backups')
    for path in [safe(root,'release-profile.json'), *sorted(cache.glob('*/release-profile.json'),reverse=True)]:
        try:
            yield read(safe(root,path.relative_to(root)))
        except (OSError,ValueError):
            continue


def programs(root):
    root=Path(root).resolve()
    result={}
    for profile in profiles(root):
        version=profile.get('release_version','')
        artifact=profile.get('program_artifact') or {}
        sha=artifact.get('sha256','')
        if not VERSION.fullmatch(version) or not re.fullmatch('[0-9a-f]{64}',sha):
            continue
        cache=safe(root,'.local-build/program-'+sha[:16])
        manifest=safe(root,(cache/'program-manifest.json').relative_to(root))
        if manifest.is_file() and digest(manifest)==profile.get('program_manifest_sha256'):
            result.setdefault(version,(artifact,cache))
    return result


def program_for(root, row, verify=False):
    for artifact,cache in programs(root).values():
        if artifact.get('sha256')!=row.get('sha256'):continue
        if verify:
            for entry in read(cache/'program-manifest.json')['files']:
                path=safe(cache,entry['path'])
                if not path.is_file() or path.stat().st_size!=entry['bytes'] or digest(path)!=entry['sha256']:
                    return None
        return str(cache)
    return None


def slot(root,locale,row):
    if locale not in ('en-US','ko-KR'):return None
    key=row.get('plan_sha256','')+row.get('elf_sha256','')
    if not re.fullmatch('[a-f0-9]{128}',key):return None
    return safe(root,'.local-build/retained-languages/'+locale+'/'+hashlib.sha256(key.encode('ascii')).hexdigest()[:24])


def retained(root,locale,row):
    storage=slot(root,locale,row)
    if not storage:return False
    try:
        recorded=read(safe(storage,'view.json'))
        if any(recorded.get(k)!=row.get(k) for k in ('plan_sha256','elf_sha256','data_id')):return False
        return valid_view(storage/'view',storage/'game.elf',recorded,locale)
    except (OSError,ValueError,KeyError):return False


def valid_view(view,elf,row,locale):
    if row.get('root')!='data/languages/'+locale or row.get('elf')!='bin/EBOOT.'+locale+'.elf':return False
    if safe(view,'data/installed-version.txt').read_text().strip()!=row['data_id'] or digest(elf)!=row['elf_sha256']:return False
    for item in row['files']:
        stat=safe(view,item['path']).stat()
        if stat.st_size!=item['size'] or stat.st_mtime_ns!=item['mtime_ns']:return False
    return bool(row['files'])


def active_paths(root,locale):
    if locale not in ('ko-KR','en-US'):raise ValueError('Unsupported retained language')
    return {'view':safe(root,'data/languages/'+locale),'game.elf':safe(root,'bin/EBOOT.'+locale+'.elf'),
            'subtitles':safe(root,'bin/subtitles/'+locale)}


def recover(root,locale):
    """Reverse an interrupted swap before inspecting or staging another runtime."""
    journal=safe(root,'.local-build/retained-languages/'+locale+'/pending.json')
    if not journal.is_file():return
    txn=read(journal)
    paths=active_paths(root,locale)
    target=slot(root,locale,txn['expected'])
    if not re.fullmatch('[a-f0-9]{32}',txn['id']):raise ValueError('Invalid recovery record')
    recovery=safe(root,'.local-build/language-recovery/'+txn['id'])
    storage=slot(root,locale,txn['old']) if txn['preserve'] else recovery/'previous'
    for name,path in paths.items():
        if txn['phase']=='activate' and name in txn['incoming'] and path.exists() and not (target/name).exists():
            path.rename(target/name)
        if txn['phase']=='install' and path.exists():
            # When resuming recovery the old file may already be back in place.
            if name not in txn['outgoing'] or (storage/name).exists():
                recovery.mkdir(parents=True,exist_ok=True)
                path.rename(recovery/name)
        if name in txn['outgoing'] and (storage/name).exists():
            (storage/name).rename(path)
    write(safe(root,'.local-build/language-views.json'),txn['state'])
    write(safe(root,'release-profile.json'),txn['profile'])
    journal.unlink()


def switch_language(root,locale,expected,install):
    root=Path(root).resolve()
    profile_path=safe(root,'release-profile.json')
    state_path=safe(root,'.local-build/language-views.json')
    if not profile_path.is_file() or not state_path.is_file():
        return install()
    profile,state=read(profile_path),read(state_path)
    old=profile.get('language_views',{}).get(locale,{})
    target=slot(root,locale,expected)
    storage=slot(root,locale,old)
    paths=active_paths(root,locale)
    restoring=retained(root,locale,expected)
    if not target or storage==target:return install()
    try:
        preserve=storage and valid_view(paths['view'],paths['game.elf'],old,locale)
    except (OSError,ValueError,KeyError):
        preserve=False
    if not preserve and not restoring:return install()
    # Keep partial input for recovery. Only generated locale files move; saves are outside these paths.
    token=uuid.uuid4().hex
    if not preserve:storage=safe(root,'.local-build/language-recovery/'+token+'/previous')
    if any((storage/k).exists() for k in paths):
        raise ValueError('Retained language destination already contains files: '+str(storage))
    storage.mkdir(parents=True,exist_ok=True)
    journal=safe(root,'.local-build/retained-languages/'+locale+'/pending.json')
    if journal.exists():raise ValueError('Finish recovering the previous language switch first')
    txn=dict(id=token,expected=expected,old=old,preserve=bool(preserve),profile=profile,state=state,
             outgoing=[name for name,path in paths.items() if path.exists()],
             incoming=[name for name in paths if (target/name).exists()],phase='park')
    write(journal,txn)
    try:
        for name,active in paths.items():
            if active.exists():
                active.rename(storage/name)
        if preserve:write(storage/'view.json',old)
        if restoring:
            txn['phase']='activate';write(journal,txn)
            row=read(target/'view.json')
            for name,active in paths.items():
                source=safe(target,name)
                if source.exists():
                    active.parent.mkdir(parents=True,exist_ok=True)
                    source.rename(active)
            write(state_path,{**state,'languages':{**state['languages'],locale:row}})
            write(profile_path,{**profile,'language_views':{**profile['language_views'],locale:row},
                'supported_languages':list(dict.fromkeys([*profile.get('supported_languages',[]),locale]))})
        else:
            txn['phase']='install';write(journal,txn)
            install()
        journal.unlink()
    except BaseException:
        recover(root,locale)
        raise
