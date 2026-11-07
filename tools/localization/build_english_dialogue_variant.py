"""Translate the separately verified gold quest-reward dialogue atlas variant."""
import struct
from english_pack import ROOT, digest, write
from build_english_ui_images import BASE, read, invoke
from stage_gop_primary_mirrors import pidx, read_payload


if __name__=='__main__':
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
    expected='c9cd217e5c04312ab5b52fb8196b7b90de55dab305d22a7959618036faec9ed3'
    candidates=[read_payload(pack/row['pack'].split('/')[-1].upper(),row)[0] for row in pidx(pack/'PACK.IDX')[1] if row['name']=='stay_tx_dialogue.dds']
    data=next(d for d in candidates if digest(d)==expected)
    asset=next(a for a in read(BASE/'hud-plan.json')['assets'] if a['name']=='stay_tx_dialogue.dds')
    if struct.unpack_from('<II',data,12)!=(128,512) or len(data)!=262272:
        raise ValueError('Unexpected gold dialogue layout')
    source=BASE/'hud-input/stay_tx_dialogue-alternate.dds';source.write_bytes(data)
    bgra=source.with_suffix('.bgra');bgra.write_bytes(data[128:])
    asset.update(source_DDS=str(source),source_sha256=expected,source_BGRA=str(bgra),source_BGRA_sha256=digest(data[128:]),output_stem=source.stem)
    write(BASE/'dialogue-variant-plan.json',dict(assets=[asset]))
    invoke('compose_misc_menu_labels.ps1',['-GlyphMap',str(BASE/'hud-map.json'),'-Plan',str(BASE/'dialogue-variant-plan.json'),'-Output',str(BASE/'dialogue-variant-composited')])
    invoke('encode_misc_menu_replacements.py',['--composition',str(BASE/'dialogue-variant-composited/composition.json'),'--plan',str(BASE/'dialogue-variant-plan.json'),'--output',str(BASE/'dialogue-variant-replacements')])
