"""Compare Korean-localized original literal starts with the complete English ELF."""
import json
from pathlib import Path
import re
from english_pack import ROOT,digest,write

if __name__=='__main__':
    base=ROOT/'localization/en-US'
    active=json.loads((ROOT/'localization/image_assets/active-ui.json').read_text(encoding='utf8'))
    bundle=ROOT/'localization/image_assets'/active['bundle']
    selected=active.get('elf_override')
    if selected:ko_path=bundle/selected['path'];expected=selected['sha256']
    else:
        ready=json.loads((bundle/'ready.json').read_text(encoding='utf8'));ko_path=bundle/ready['elf']['path'];expected=ready['elf']['sha256']
    assert ko_path.resolve().is_relative_to(bundle.resolve())
    korean=ko_path.read_bytes();assert digest(korean)==expected
    english=(base/'build/EBOOT.full-english.elf').read_bytes()
    original=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    assert len(original)==len(korean)==len(english)
    candidates=json.loads((ROOT/'analysis/localization/elf_japanese_candidates.json').read_text(encoding='utf8'))
    jp=re.compile(r'[\u3041-\u3096\u30a1-\u30fa\u3400-\u9fff]');hangul=re.compile(r'[\uac00-\ud7af]')
    missing=[];covered=[]
    def text(data,offset):
        try:return data[offset:data.index(b'\0',offset)].decode('utf8')
        except (ValueError,UnicodeDecodeError):return None
    for row in candidates['entries']:
        offset=row['file_offset']
        if offset<0x900000:continue
        assert text(original,offset)==row['source']
        ko=text(korean,offset);en=text(english,offset)
        if not ko or not hangul.search(ko):continue
        target=dict(offset=hex(offset),original=row['source'],korean=ko,english=en)
        if en is None or jp.search(en) or hangul.search(en):missing.append(target)
        else:covered.append(target)
    report=dict(korean_elf=str(ko_path),korean_sha256=expected,english_sha256=digest(english),covered=len(covered),missing=missing,
                limitation='Literal-start parity is an inventory check; typed-pointer pool readback and fixed-record verification remain separate required checks.')
    write(base/'build/elf-korean-parity.json',report)
    print(json.dumps(dict(covered=len(covered),missing=len(missing),examples=missing[:15]),ensure_ascii=False))
