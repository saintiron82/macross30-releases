"""Build typed aircraft/weapon/skill display literals through the proven slot pool."""
import copy
import json
from pathlib import Path
from english_pack import ROOT,digest,write
from build_ui_catalog_pool import build
from stage_gop_primary_mirrors import pidx,read_payload
from audit_runtime_font import read_font
from english_font import original_font
from english_sidecar_results import load_results
from patch_combat_notifications_v14 import transform

def read(path):return json.loads(path.read_text(encoding='utf8'))

def lines_like(source,text):
    count=source.count('\n')+1
    words=text.replace(chr(92)+'n',' ').split();parts=[]
    for n in range(count):
        take=round(len(words)/(count-n));parts.append(' '.join(words[:take]));words=words[take:]
    return '\n'.join(parts)

if __name__=='__main__':
    base=ROOT/'localization/en-US';scope,results=load_results()
    assert results['scope_sha256']==digest((base/'model-drafts/sidecar-scope.json').read_bytes())
    inventory=copy.deepcopy(read(ROOT/'localization/image_assets/data-ui-v6/catalog-translations-integrated.json'))
    original=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    startup=(base/'build/EBOOT.preview-v4.elf').read_bytes()
    assert digest(original)==inventory['source_sha256'] and len(startup)==len(original)
    expected=read(base/'build/startup-v4-manifest.json')['output_sha256']
    startup,common_changes=transform(startup,[(0x909ce0,'%sの%sが発動！',"%s's %s activated!"),
                                            (0x938b28,'扉が開放された','Door opened.')],set(range(32,127)),expected)
    jobs={r['file_offset']:r for r in scope['jobs'] if r['kind']=='elf'}
    known={}
    for p in (base/'structured').glob('*.json'):
        for r in read(p)['entries']:
            if r['translation']:known.setdefault(r['source'],r['translation'])
    missing=[]
    for row in inventory['entries']:
        if row['file_offset'] not in jobs:continue
        job=jobs[row['file_offset']];record=results['entries'].get(job['id'])
        if not record:missing.append(job['id']);continue
        assert record['source']==row['source']
        row['translation_draft']=lines_like(row['source'],known.get(row['source'],record['translation']))
        row['translation_provenance']='English source-checked local draft; English GOP vocabulary reused where exact'
    if missing:raise ValueError(f'{len(missing)} English ELF drafts missing: {missing[:8]}')
    font,_,glyphs,_=original_font();repertoire=set(glyphs)
    output,proof=build(original,startup,inventory,repertoire)
    target=base/'build/EBOOT.full-english.elf';target.write_bytes(output)
    proof.update(elf=str(target),english_catalog_literals=len(jobs),original_font_sha256=digest(font),
                 manual_startup_slots=336,pilot_name_records=139,extra_common_ui_changes=common_changes,model_translation_review='pending',live_game_verified=False)
    write(base/'build/full-elf-manifest.json',proof)
    print(json.dumps({k:proof[k] for k in ('elf','translated_literals','typed_pointer_count','output_sha256')}))
