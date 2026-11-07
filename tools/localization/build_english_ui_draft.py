"""Build an independent English ELF draft from pinned Japanese exact string slots."""
import argparse
import hashlib
import json
from pathlib import Path
from patch_combat_notifications_v14 import transform
ROOT=Path(__file__).resolve().parents[2]
SOURCE_SHA='44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'

def build(source, entries):
    slots=[]
    for row in entries:
        text=row['translation']
        if not text or any(ord(c)<32 or ord(c)>126 for c in text):
            raise ValueError('English ELF draft requires printable ASCII')
        slots.append((int(row['file_offset'],0),row['source'],text))
    return transform(source,slots,set(range(32,127)),SOURCE_SHA)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog',type=Path,required=True)
    args=parser.parse_args()
    source=ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    catalog=json.loads(args.catalog.read_text(encoding='utf8'))
    entries=catalog['strings']
    data,changes=build(source.read_bytes(),entries)
    out=ROOT/'localization/en-US/build';out.mkdir(parents=True,exist_ok=True)
    target=out/'EBOOT.ui-en.elf'
    target.write_bytes(data)
    report=dict(locale='en-US',status='PARTIAL_DRAFT_NOT_SELECTED',source_sha256=SOURCE_SHA,
        catalog_sha256=hashlib.sha256(args.catalog.read_bytes()).hexdigest(),
        output_sha256=hashlib.sha256(data).hexdigest(),translated_slots=len(changes),
        changes=changes,source_modified=False,korean_pack_modified=False,
        outside_slot_changes=0,game_render_verified=False,
        limitations=['Untranslated slots remain Japanese.','Image resources and movie/battle subtitle catalogs are separate work.','Do not launch against Korean resource packs as a complete English edition.'])
    (out/'manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print(json.dumps({k:report[k] for k in ('locale','status','translated_slots','output_sha256')}))
if __name__=='__main__':main()
