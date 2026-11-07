"""Select a guarded staged native build without overwriting a running EXE."""
import hashlib
import json
import re
from pathlib import Path


def sha_file(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(4*1024*1024), b''): h.update(block)
    return h.hexdigest()


def select_native_binary(root, candidate_dir=None):
    root = Path(root).resolve()
    base = root/'native/build/Macross30Recompiled.exe'
    folder = root/'native/subtitle-candidate'
    if candidate_dir is not None:
        if not re.fullmatch(r'[a-z0-9-]+',candidate_dir):
            raise ValueError('Invalid diagnostic candidate name')
        directory=(folder/candidate_dir).resolve()
        if directory.parent!=folder.resolve():raise ValueError('Diagnostic candidate outside candidate folder')
        result=json.loads((directory/'build-result.json').read_text(encoding='utf8'))
        if result.get('status')!='compiled_and_linked_candidate_only':
            raise ValueError('Diagnostic candidate build incomplete')
        product=result['products']['exe'];candidate=directory/'Macross30Recompiled.exe'
        if Path(product['path']).resolve()!=candidate or sha_file(candidate)!=product['sha256']:
            raise ValueError('Diagnostic candidate checksum/path changed')
        if result['original_outputs'].get(str(base))!=sha_file(base):
            raise ValueError('Diagnostic candidate normal-build epoch changed')
        return candidate
    pointer = folder/'active.json'
    if not pointer.exists(): return base
    doc = json.loads(pointer.read_text(encoding='utf-8'))
    if doc.get('status') != 'ready': raise ValueError('Native candidate is not ready')
    # A newer normal native build supersedes this staged build.
    if sha_file(base) != doc['base_exe_sha256']: return base
    candidate = (folder/doc['candidate']).resolve()
    if folder.resolve() not in candidate.parents or candidate.name != 'Macross30Recompiled.exe':
        raise ValueError('Native candidate must stay inside the candidate folder')
    if not candidate.is_file() or sha_file(candidate) != doc['candidate_sha256']:
        raise ValueError('Native subtitle candidate hash mismatch')
    for row in doc.get('dependencies', []):
        path = (candidate.parent/row['name']).resolve()
        if path.parent != candidate.parent or sha_file(path) != row['sha256']:
            raise ValueError('Candidate dependency changed')
    return candidate


def battle_subtitles_default(root, selected):
    root=Path(root).resolve()
    pointer=root/'native/subtitle-candidate/active.json'
    if not pointer.exists():return False
    doc=json.loads(pointer.read_text(encoding='utf-8'))
    candidate=(pointer.parent/doc['candidate']).resolve()
    return (candidate==Path(selected).resolve() and
            doc.get('battle_bridge_enabled_by_default') is True and
            doc.get('bridge_live_game_verified',doc.get('live_game_verified')) is True)
