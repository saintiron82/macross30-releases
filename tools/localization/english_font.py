"""Read the actual untouched Japanese/Latin font from the original resident archive."""
from functools import lru_cache
from pathlib import Path
from english_pack import ROOT,digest
from stage_gop_primary_mirrors import pidx,read_payload,fsts_entries
from audit_runtime_font import read_font

@lru_cache(maxsize=1)
def original_font():
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
    row=next(r for r in pidx(pack/'PACK.IDX')[2] if r['name']=='resident')
    archive=read_payload(pack/Path(row['pack']).name.upper(),row)[0]
    entries=[r for r in fsts_entries(archive) if r['name']=='data/font/font_j24x24.abf']
    assert len(entries)==1
    data=entries[0]['payload'];common,glyphs,pages=read_font(data)
    return data,common,{r['codepoint']:r for r in glyphs},pages
