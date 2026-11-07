"""Patch the exact fixed launch prompt, retaining old localized ELF files."""
import hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
OFFSET=0x8f1068
def sha(b):return hashlib.sha256(b).hexdigest()
def patch(source,target,text):
    before=source.read_bytes();old='発進'.encode('utf8');new=text.encode('utf8')
    assert len(new)==len(old)==6 and before[OFFSET:OFFSET+7]==old+b'\0'
    after=before[:OFFSET]+new+before[OFFSET+6:]
    assert after[:OFFSET]==before[:OFFSET] and after[OFFSET+6:]==before[OFFSET+6:]
    assert not target.exists();target.write_bytes(after)
    assert target.read_bytes()==after
    return dict(path=target.name,sha256=sha(after),base_elf_sha256=sha(before),offset=hex(OFFSET),source='発進',translation=text,changed_bytes=6,game_render_verified=False)
def main():
    pointer=ROOT/'localization/image_assets/active-ui.json';active=json.loads(pointer.read_text(encoding='utf8'))
    bundle=ROOT/'localization/image_assets'/active['bundle'];ready=json.loads((bundle/'ready.json').read_text(encoding='utf8'))
    assert 'elf_override' not in active
    ko=patch(bundle/ready['elf']['path'],bundle/'EBOOT.launch-prompt-v22.elf','출격')
    assert ko['base_elf_sha256']==ready['elf']['sha256']
    (bundle/'launch-prompt-v22.json').write_text(json.dumps(ko,ensure_ascii=False,indent=2),encoding='utf8')
    active['elf_override']={k:ko[k] for k in ('path','sha256','base_elf_sha256')}
    pointer.write_text(json.dumps(active,indent=2),encoding='utf8')
    en_pointer=ROOT/'localization/en-US/pack-ready.json';en=json.loads(en_pointer.read_text(encoding='utf8'))
    old=ROOT/en['elf'];target=old.parent/'EBOOT.english-launch-v22.elf';proof=patch(old,target,'Launch')
    (old.parent/'launch-prompt-v22.json').write_text(json.dumps(proof,ensure_ascii=False,indent=2),encoding='utf8')
    en['elf']=target.relative_to(ROOT).as_posix()
    for p in en['proofs']:
        if p['path'].replace('\\','/')==old.relative_to(ROOT).as_posix():p.update(path=en['elf'],sha256=proof['sha256'])
    preview=old.parent/'preview.json';doc=json.loads(preview.read_text(encoding='utf8'))
    doc['command']=[str(target) if s==str(old) else s for s in doc['command']]
    preview.write_text(json.dumps(doc,ensure_ascii=False,indent=2),encoding='utf8')
    for p in en['proofs']:
        if p['path'].replace('\\','/')==preview.relative_to(ROOT).as_posix():p['sha256']=sha(preview.read_bytes())
    en_pointer.write_text(json.dumps(en,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(dict(status='PASS',offset=hex(OFFSET),ko=ko['sha256'],en=proof['sha256'],old_files_preserved=True,restart_required=True)))
if __name__=='__main__':main()
