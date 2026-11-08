"""Stage an unpublished RU layout candidate against a verified 0.3.10 text base."""
import argparse,hashlib,json,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(b):return hashlib.sha256(b).hexdigest()
def prepare(baseline,output):
    if output.exists():raise ValueError('Fresh output required')
    boot=json.loads((baseline/'BOOT-INSTALL.json').read_text(encoding='utf8'));assert boot['version']=='0.3.10'
    row=boot['locale_packs']['ru-RU'];source=baseline/row['file'];assert sha(source.read_bytes())==row['sha256']
    with zipfile.ZipFile(source) as z:old={n:z.read(n) for n in z.namelist() if not n.endswith('/')}
    v5=ROOT/'final/language-packs/ru-RU-full-text-v5';v6=ROOT/'final/language-packs/ru-RU-full-text-v6-r2'
    expected={p.relative_to(v5).as_posix():p.read_bytes() for p in v5.rglob('*') if p.is_file()}
    assert {n:d for n,d in old.items() if n!='pack.json'}=={n:d for n,d in expected.items() if n!='pack.json'},'Public 0.3.10 payload differs from known v5'
    files={p.relative_to(v6).as_posix():p.read_bytes() for p in v6.rglob('*') if p.is_file()}
    doc=json.loads(files['pack.json']);public=json.loads(old['pack.json']);doc['source_guards']=public['source_guards']
    files['pack.json']=(json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode('utf8')
    changes=sorted(n for n in files if files[n]!=old[n]);allowed={'pack.json','coverage.json','elf-authored.json','elf-text.pack','elf-text.pack.json','gop/gop_guide.gop'}
    assert set(changes)==allowed
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'x',zipfile.ZIP_DEFLATED) as z:
        for n,data in sorted(files.items()):z.writestr(n,data)
    result=dict(ok=True,public_published=False,installed=False,game_started=False,baseline_version='0.3.10',text_revision='ru-layout-v6-r2',source_zip=str(source),source_sha256=row['sha256'],output=str(output),sha256=sha(output.read_bytes()),bytes=output.stat().st_size,manifest_sha256=sha(files['pack.json']),source_guards=doc['source_guards'],changed_members=changes,font_and_captions_unchanged=True,other_languages_unchanged=True,program_update_required=False,coverage=doc['coverage'])
    output.with_suffix('.audit.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf8');return result
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(json.dumps(prepare(a.baseline,a.output),ensure_ascii=False,indent=2))
