"""Create a separate v14 English pack with the prepared nested viewer guide."""
import hashlib,json,shutil,struct
from datetime import datetime,timezone
from pathlib import Path
from stage_gop_primary_mirrors import pidx
from stage_nested_ui import region,fsts_entries,decoded,rebuild
ROOT=Path(__file__).resolve().parents[2]
def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
    return h.hexdigest()
def main():
    load=lambda p:json.loads(p.read_text(encoding='utf8'))
    pointer=ROOT/'localization/en-US/pack-ready.json';ready=load(pointer)
    old=ROOT/ready['vfs'];assert old.name=='english-preview-images-v13'
    new=ROOT/'build/english-preview-images-v14';state=ROOT/'native/state-en-preview-images-v14'
    assert not new.exists() and not state.exists()
    shutil.copytree(old,new);shutil.copytree(ROOT/ready['state'],state)
    a=load(ROOT/'localization/viewer-guide/manifest.json')['en-US'];replacement=Path(a['replacement']).read_bytes()
    assert hashlib.sha256(replacement).hexdigest()==a['replacement_sha256']
    disc=new/'PS3_GAME/USRDIR/DATA/PACK';hdd=state/'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
    _,_,archives=pidx(disc/'PACK.IDX');proof=[];changed={'PACK.IDX'}
    for archive in archives:
        if archive['name'].lower()!='hangar':continue
        pack=disc/Path(archive['pack']).name
        blob=region(pack,archive['offset'],archive['size']);patches={}
        for e in fsts_entries(blob):
            if e['path']!=a['nested_path']:continue
            raw,_=decoded(blob,e);assert hashlib.sha256(raw).hexdigest()==a['source_sha256']
            patches[e['index']]=(a['source_sha256'],replacement)
        assert len(patches)==1;rebuilt,entries=rebuild(blob,patches)
        _,_,embedded=pidx(pack);aliases=[r for r in embedded if all(r[k]==archive[k] for k in ('name','pack','offset','size'))];assert len(aliases)==1
        with pack.open('ab') as f:f.write(b'\0'*(-f.tell()%2048));off=f.tell();f.write(rebuilt)
        for path,row in [(disc/'PACK.IDX',archive),(pack,aliases[0])]:
            with path.open('r+b') as f:f.seek(row['record']+8);f.write(struct.pack('<II',off,len(rebuilt)))
        changed.add(pack.name);proof+=entries
    assert len(proof)==1
    for name in changed:shutil.copyfile(disc/name,hdd/name)
    for folder in (disc,hdd):
        _,_,archives=pidx(folder/'PACK.IDX');r=next(r for r in archives if r['name'].lower()=='hangar')
        blob=region(folder/Path(r['pack']).name,r['offset'],r['size'])
        e=next(e for e in fsts_entries(blob) if e['path']==a['nested_path'])
        assert decoded(blob,e)[0]==replacement
    previous_state=ROOT/ready['state']
    manifest=load(new/'preview.json');old_run=Path(manifest['run_directory'])
    run=ROOT/'native/analysis'/('english-viewer-v14-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'));run.mkdir()
    if (old_run/'gamedata').exists():shutil.copytree(old_run/'gamedata',run/'gamedata')
    def replace(s):return s.replace(str(old),str(new)).replace(str(previous_state),str(state)).replace(str(old_run),str(run))
    manifest['command']=[replace(s) for s in manifest['command']]
    manifest['command'][manifest['command'].index('--candidate-dir')+1]=ready['candidate_dir']
    manifest['run_directory']=str(run);manifest['translated_image_variants']+=1
    manifest['checks'].append(dict(name=a['name'],route='nested',path=a['nested_path'],sha256=a['replacement_sha256']))
    (new/'preview.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
    (new/'viewer-guide-repair.json').write_text(json.dumps(dict(status='PASS',disc_and_hdd_readback=True,entries=proof,live_game_verified=False),indent=2),encoding='utf8')
    (new/'previous-pack-ready.json').write_text(json.dumps(ready,indent=2),encoding='utf8')
    ready['vfs']=str(new.relative_to(ROOT)).replace('\\','/');ready['elf']=str((new/'EBOOT.english.elf').relative_to(ROOT)).replace('\\','/');ready['state']=str(state.relative_to(ROOT)).replace('\\','/')
    for p in ready['proofs']:
        path=ROOT/p['path']
        if path.is_relative_to(old):
            path=new/path.relative_to(old);p['path']=str(path.relative_to(ROOT)).replace('\\','/');p['sha256']=sha(path)
    proof_path=new/'viewer-guide-repair.json';ready['proofs'].append(dict(path=str(proof_path.relative_to(ROOT)).replace('\\','/'),sha256=sha(proof_path)))
    pointer.write_text(json.dumps(ready,ensure_ascii=False,indent=2),encoding='utf8')
    (ROOT/'localization/en-US/preview-active.json').write_text(json.dumps(dict(manifest=str(new/'preview.json')),indent=2),encoding='utf8')
    print(json.dumps(dict(status='PASS',version='v14',disc_and_hdd_readback=True,prior_version_preserved=True)))
if __name__=='__main__':main()
