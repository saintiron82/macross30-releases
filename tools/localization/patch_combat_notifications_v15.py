"""Extend the frozen v14 notification ELF with skill/door notices; prepare only."""
import json
from pathlib import Path
from audit_runtime_font import read_font
from patch_combat_notifications_v14 import BUNDLE, FONT_SHA, transform, sha, self_tests

SOURCE_SHA='dcae539cdaec8fa056b7f237389e98d430ddb0faf44ccdf2c86647482ec70a45'
SLOTS=((0x909CE0,'%sの%sが発動！','%s의 %s 발동!'),
       (0x938B28,'扉が開放された','문이 열렸다'))


def main():
    self_tests()
    source_path=BUNDLE/'EBOOT.combat-notifications-v14.elf'
    source=source_path.read_bytes()
    authority=json.loads((BUNDLE/'candidate/authority.json').read_text(encoding='utf8'))
    font=next(r for r in authority['entries'] if r['type']=='ABF')
    font_bytes=Path(font['replacement']).read_bytes();assert sha(font_bytes)==FONT_SHA
    _,glyphs,_=read_font(font_bytes)
    output,changes=transform(source,SLOTS,{r['codepoint'] for r in glyphs},SOURCE_SHA)
    target=BUNDLE/'EBOOT.combat-notifications-v15.elf'
    if target.exists() and target.read_bytes()!=output:raise ValueError('Preserve differing v15 output')
    target.write_bytes(output)
    prior=BUNDLE/'combat-notifications-v14.manifest.json'
    report=dict(status='prepared; selection and game-render verification separate',
        source_sha256=SOURCE_SHA,output_sha256=sha(output),source_path=str(source_path),output_path=str(target),
        inherited_entries=13,additional_entries=2,total_entries=15,
        inherited_manifest_sha256=sha(prior.read_bytes()),changes=changes,
        file_size_unchanged=len(source)==len(output),byte_changes_outside_reviewed_slots=0,
        printf_identity_order_width_preserved=True,font_ABF_sha256=FONT_SHA,
        self_tests='PASS',running_game_modified=False,in_game_verified=False)
    (BUNDLE/'combat-notifications-v15.manifest.json').write_bytes((json.dumps(report,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
    print(json.dumps(dict(total_entries=15,output_sha256=report['output_sha256'],game_modified=False)))


if __name__=='__main__':main()
