"""Official release selection and safe changes of the installed game version."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import urllib.request

REPO = 'saintiron82/macross30-releases'
VERSION = re.compile(r'\d+\.\d+\.\d+\Z')


def selection(value):
    if value == 'latest' or isinstance(value, str) and VERSION.fullmatch(value):
        return value
    raise ValueError('Invalid game release selection')


def manifest_url(value):
    value = selection(value)
    if value == 'latest':
        return f'https://raw.githubusercontent.com/{REPO}/main/channels/stable/BOOT-INSTALL.json'
    return f'https://github.com/{REPO}/releases/download/v{value}/BOOT-INSTALL.json'


def catalog(opener):
    request = urllib.request.Request(f'https://api.github.com/repos/{REPO}/releases?per_page=100',
        headers={'User-Agent':'Macross30Launcher', 'Accept':'application/vnd.github+json'})
    with opener(request, timeout=25) as response:
        raw=response.read(2*1024*1024+1)
    if len(raw)>2*1024*1024:raise ValueError('Release list too large')
    rows=json.loads(raw.decode('utf-8-sig'))
    if not isinstance(rows,list):raise ValueError('Invalid release list')
    versions=set()
    for row in rows:
        tag=row.get('tag_name','')
        if row.get('draft') or row.get('prerelease') or not tag.startswith('v') or not VERSION.fullmatch(tag[1:]):continue
        # Launcher-only tags cannot safely select a game/language combination.
        names={a.get('name') for a in row.get('assets',[])}
        if 'BOOT-INSTALL.json' not in names or f'Macross30-program-{tag[1:]}.zip' not in names:continue
        versions.add(tag[1:])
    return ['latest']+sorted(versions,key=lambda s:tuple(map(int,s.split('.'))),reverse=True)


def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def safe(root, relative):
    root=Path(root).resolve();path=root/relative
    if Path(relative).is_absolute() or '..' in Path(relative).parts or not path.resolve().is_relative_to(root):
        raise ValueError('Version resource escaped installation')
    for parent in (path,*path.parents):
        if parent.is_symlink() or getattr(parent,'is_junction',lambda:False)():raise ValueError('Linked version resource')
    return path


def begin_transition(root, new_version):
    """Back up small user state before a different runtime is installed."""
    root=Path(root).resolve();selection(new_version)
    path=safe(root,'release-profile.json')
    previous=json.loads(path.read_text('utf-8-sig')) if path.exists() else {}
    old=previous.get('release_version','')
    if old==new_version or not previous:return previous
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    backup=safe(root,'.local-build/version-switch-backups/'+stamp)
    backup.mkdir(parents=True)
    for rel in ['release-profile.json','Macross30.cmd','play/settings.ini']:
        source=safe(root,rel)
        if source.is_file():
            target=backup/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
    for rel in ['play/gamedata','data/hdd0/home']:
        source=safe(root,rel)
        if source.exists():
            for current,dirs,files in os.walk(source,followlinks=False):
                for name in dirs+files:safe(root,(Path(current)/name).relative_to(root))
            shutil.copytree(source,backup/rel)
    (backup/'transition.json').write_text(json.dumps(dict(from_version=old,to_version=new_version,
        saves_preserved=True),indent=2)+'\n',encoding='utf8')
    previous['_version_backup']=backup.relative_to(root).as_posix()
    return previous


def sync_bootstrap(root, program, previous=None):
    """Use the target version's command and remove only verified stale program files."""
    root,program=Path(root).resolve(),Path(program).resolve()
    manifest=json.loads((program/'program-manifest.json').read_text('utf-8-sig'))
    rows={r['path']:r for r in manifest['files']}
    template=program/'base-game/Macross30.cmd'
    expected=rows.get('base-game/Macross30.cmd')
    if not expected or digest(template)!=expected['sha256']:
        raise ValueError('Target release launch command is not verified')
    from prebuilt_finalize import launcher_command
    generated=launcher_command(template.read_text('utf-8-sig'))
    retired=[]
    for row in (previous or {}).get('program_files',[]):
        name=row.get('path','')
        if not name.startswith('bin/') or name in rows:continue
        source=safe(root,name)
        if source.exists():
            if digest(source)!=row['sha256']:raise ValueError('A previous program file was modified: '+name)
            retired.append((name,source))
    target=safe(root,'Macross30.cmd')
    if not target.exists() or target.read_bytes()!=generated:
        backup=safe(root,'.local-build/launch-command-backups/'+digest(target)+'.cmd') if target.exists() else None
        if backup and not backup.exists():backup.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(target,backup)
        pending=target.with_suffix('.pending');pending.write_bytes(generated);os.replace(pending,target)
    removed=[]
    if retired and not previous.get('_version_backup'):
        previous['_version_backup']='.local-build/version-switch-backups/'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    for name,source in retired:
        destination=safe(root,(previous['_version_backup']+'/retired-program/'+name))
        destination.parent.mkdir(parents=True,exist_ok=True);source.replace(destination);removed.append(name)
    return dict(command_sha256=hashlib.sha256(generated).hexdigest(),retired_program_files=removed)
