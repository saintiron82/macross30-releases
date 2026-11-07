"""Verify GDI alpha-only atlas and emit provenance. Does not edit images."""
import hashlib
import json
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import validate_dds


def main():
    root = Path('localization/image_assets/caution-ui-v4')
    source = Path('native/analysis/combat-caution-ui/ope60_title.dds')
    target = root/'replacements/ope60_title.dds'
    old, new = source.read_bytes(), target.read_bytes()
    assert hashlib.sha256(old).hexdigest() == '2f2d5f14f687a822bf183abef14c4c245083628d90413ca77f54eddf51e314f5'
    validate_dds(old); validate_dds(new)
    assert len(old) == len(new) and old[:128] == new[:128]
    changes = [i for i,(a,b) in enumerate(zip(old,new)) if a != b]
    assert changes and all(128 <= i < 128+512*384*4 and (i-128)%4 == 3 for i in changes)
    assert old[128+512*384*4:] == new[128+512*384*4:]
    mapping = json.loads(Path('native/analysis/combat-caution-ui/ope60-glyph-rois.json').read_text(encoding='utf8'))
    crops = json.loads((root/'replacements/gdi-glyph-crops.json').read_text())
    for row, crop in zip(mapping['glyphs'],crops):
        n = row['index']; assert n == crop['index']
        x,y,w,h = row['source_cell_xywh']; assert [x,y,w,h] == [(n%4)*128,(n//4)*96,128,96]
        pixels = [new[128+((y+dy)*512+x+dx)*4+3] for dy in range(h) for dx in range(w)]
        assert sum(a >= 192 for a in pixels) > 200
        row['gdi_crop'] = crop
    provenance = next(r for r in json.loads(Path('native/analysis/combat-caution-ui/source-manifest.json').read_text()) if r['name']=='ope60_title.dds')
    doc = dict(source=str(source),output=str(target),source_sha256=hashlib.sha256(old).hexdigest(),
               sha256=hashlib.sha256(new).hexdigest(),glyph_source=str(root/'source-glyphs.png'),
               glyph_source_sha256=hashlib.sha256((root/'source-glyphs.png').read_bytes()).hexdigest(),
               original_inventory=provenance['inventory'],width=512,height=512,format='BGRA32 single mip',
               changed_alpha_bytes=len(changes),header_changed_bytes=0,rgb_changed_bytes=0,
               lower_blank128_rows_changed_bytes=0,per_character_uv_unchanged=True,
               cell_layout='4 columns x 4 rows; each cell128x96; y384..511 untouched',
               mappings=mapping['glyphs'],source_alpha_rule='minRGB*alpha/255; values<64 removed; core crop >=192',
               visual_review='All16 Korean syllables checked; 료 enlarged to confirm two ㅛ stems, not 로',
               phrase_proof_rows=['전투개시','전투종료','임무종료','목표달성','임무실패','계속'],
               proof_is_game_capture=False,english_other_assets_modified=False,active_modified=False,in_game_verified=False)
    (root/'replacements/ope60_title.manifest.json').write_text(json.dumps(doc,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in doc.items() if k not in ('mappings','original_inventory')},ensure_ascii=True))


if __name__ == '__main__':main()
