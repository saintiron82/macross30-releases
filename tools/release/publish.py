"""Publish a verified stage, preserving existing tags and unrelated stable components."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import urllib.request

from pipeline import META, read, sha, verify, write

REPO = 'saintiron82/macross30-releases'


def command(argv, cwd=None):
    return subprocess.check_output(argv, cwd=cwd).decode('utf-8').strip()


def verify_public(output):
    output = Path(output).resolve(strict=True)
    proof = verify(output)
    plan = read(output/'release-plan.json')
    tag = 'v'+proof['release']
    release = json.loads(command(['gh','api','repos/'+REPO+'/releases/tags/'+tag]))
    if release['draft'] or release['tag_name'] != tag:
        raise ValueError('Release is not published')
    expected = {r['file']:r for r in plan['artifacts']}
    actual = {r['name']:r for r in release['assets']}
    if set(actual) != set(expected): raise ValueError('Public inventory mismatch')
    for name,row in expected.items():
        if actual[name]['size'] != row['bytes'] or actual[name].get('digest') != 'sha256:'+row['sha256']:
            raise ValueError('Public server hash mismatch: '+name)
        digest, total = hashlib.sha256(), 0
        url = 'https://github.com/'+REPO+'/releases/download/'+tag+'/'+name
        with urllib.request.urlopen(url,timeout=60) as response:
            while block := response.read(1 << 20):
                total += len(block)
                if total > row['bytes']: raise ValueError('Public asset larger than expected: '+name)
                digest.update(block)
        if total != row['bytes'] or digest.hexdigest() != row['sha256']:
            raise ValueError('Anonymous public download hash mismatch: '+name)
    report = dict(verified=True,tag=tag,assets=len(expected),anonymous_downloads_verified=True,url=release['html_url'])
    write(output/'public-verification.json',report)
    return report


def publish(output, checkout):
    output, checkout = Path(output).resolve(strict=True), Path(checkout).resolve(strict=True)
    proof = verify(output)
    plan = read(output/'release-plan.json')
    tag = 'v'+proof['release']
    state = output/'publication.json'
    if state.exists():
        raise ValueError('Publication has an existing receipt; inspect it before recovery. No duplicate publish was started.')
    def git(*args): return command(['git',*args], cwd=checkout)
    origin = git('remote','get-url','origin')
    if origin not in ('https://github.com/'+REPO+'.git','https://github.com/'+REPO,'git@github.com:'+REPO+'.git'):
        raise ValueError('Checkout is not the public release repository')
    if git('status','--porcelain'): raise ValueError('Use a clean, dedicated public metadata checkout')
    if git('ls-remote','--tags','origin','refs/tags/'+tag):
        raise ValueError('Tag already exists; published releases are immutable: '+tag)
    git('fetch','origin','main')
    head = git('rev-parse','origin/main')
    # Read baseline bytes directly from fetched main before touching any branch.
    for name, expected in plan['baseline_hashes'].items():
        raw = git('show','origin/main:channels/stable/'+name)
        # Git checkout line-ending conversion is allowed; metadata must be equal.
        if json.loads(raw) != read(Path(plan['baseline_metadata'])/name):
            raise ValueError('Stable baseline changed; prepare a new release plan: '+name)
        if sha(Path(plan['baseline_metadata'])/name) != expected:
            raise ValueError('Local baseline changed: '+name)
    # The baseline may be this checkout: capture versions before replacing its files.
    old_launcher = read(Path(plan['baseline_metadata'])/'PYTHON-LAUNCHER.json')['version']
    old_game = read(Path(plan['baseline_metadata'])/'BOOT-INSTALL.json')['version']
    branch = 'release/'+proof['release']
    git('switch','-c',branch,'origin/main')
    for name in META:
        shutil.copyfile(output/'assets'/name,checkout/'channels/stable'/name)
    # Replace only existing launcher download references; preserve game instructions.
    if 'launcher' in plan['changed']:
        old = old_launcher
        new = plan['launcher']['version']
        for name in ('README.md','README.en.md'):
            path = checkout/name
            if path.exists():
                text = path.read_text(encoding='utf-8-sig')
                text = text.replace('/download/v'+old+'/Macross30-launcher-'+old+'.zip',
                                    '/download/v'+new+'/Macross30-launcher-'+new+'.zip')
                text = text.replace('Macross30-launcher-'+old+'.zip','Macross30-launcher-'+new+'.zip')
                text = text.replace('Launcher **'+old+'**','Launcher **'+new+'**')
                text = text.replace('Launcher '+old,'Launcher '+new).replace('런처 '+old,'런처 '+new)
                path.write_text(text,encoding='utf-8')
    if 'game' in plan['changed']:
        old = old_game
        new = plan['boot']['version']
        for name in ('README.md','README.en.md'):
            path = checkout/name
            if path.exists():
                text = path.read_text(encoding='utf-8-sig')
                text = text.replace('Game **'+old+'**','Game **'+new+'**')
                path.write_text(text,encoding='utf-8')
    for name,text in plan['config'].get('readme_addendum',{}).items():
        if name not in ('README.md','README.en.md'):raise ValueError('Unexpected public documentation target')
        path=checkout/name
        existing=path.read_text(encoding='utf-8-sig')
        if text not in existing:path.write_text(existing.rstrip()+'\n\n'+text.strip()+'\n',encoding='utf-8')
    git('add','--',*[str(Path('channels/stable')/name) for name in META],'README.md','README.en.md')
    git('commit','-m','Release '+proof['release']+': '+', '.join(plan['changed']))
    commit = git('rev-parse','HEAD')
    receipt = dict(stage='metadata_committed',tag=tag,branch=branch,commit=commit,baseline=head,repo=REPO)
    write(state,receipt)
    git('tag','-a',tag,'-m','Macross release '+proof['release']+'; changed: '+', '.join(plan['changed']))
    git('push','origin','HEAD:refs/heads/'+branch,'refs/tags/'+tag)
    assets = [str(output/'assets'/r['file']) for r in plan['artifacts']]
    command(['gh','release','create',tag,'--repo',REPO,'--draft','--verify-tag',
             '--title','Macross components '+proof['release']+' — '+', '.join(plan['changed']),
             '--notes-file',str(output/'assets/RELEASE-NOTES.txt'),*assets])
    releases = json.loads(command(['gh','api','repos/'+REPO+'/releases?per_page=100']))
    release = next(r for r in releases if r['tag_name']==tag)
    receipt.update(stage='draft_uploaded',release_id=release['id']);write(state,receipt)
    expected = {r['file']:r for r in plan['artifacts']}
    actual = {r['name']:r for r in release['assets']}
    if set(actual) != set(expected): raise ValueError('Draft upload inventory mismatch; draft remains unpublished')
    for name,row in expected.items():
        if actual[name]['size'] != row['bytes'] or actual[name].get('digest') != 'sha256:'+row['sha256']:
            raise ValueError('Draft server hash mismatch: '+name)
    # Refuse to advance a stale stable branch; never force-push recovery.
    git('fetch','origin','main')
    if git('rev-parse','origin/main') != head:
        raise ValueError('Stable changed while uploading; draft retained for recovery')
    command(['gh','release','edit',tag,'--repo',REPO,'--draft=false','--latest'])
    receipt['stage']='release_published';write(state,receipt)
    git('push','origin','HEAD:main')
    receipt.update(stage='stable_updated',url='https://github.com/'+REPO+'/releases/tag/'+tag,
                   versions=proof['versions'],server_hashes_verified=True)
    write(state,receipt)
    receipt['public_verification'] = verify_public(output)
    receipt['stage'] = 'verified';write(state,receipt)
    return receipt
