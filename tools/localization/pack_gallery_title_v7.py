"""DDS codec/byte validation only; Korean compositing is performed by GDI+."""
import hashlib
import json
from pathlib import Path
import struct
from build_quest_title_bc3 import png
from stage_ui_images import validate_dds

ROOT=Path(__file__).resolve().parents[2]
sha=lambda b:hashlib.sha256(b).hexdigest()


def validate(original,new,allowed):
    validate_dds(original);validate_dds(new)
    if len(original)!=len(new) or original[:128]!=new[:128]:raise ValueError('DDS header/length changed')
    changed={n for n,(a,b) in enumerate(zip(original,new)) if a!=b}
    if changed-allowed:raise ValueError('Byte changed outside reviewed glyph/UV mask')
    return len(changed)


def main():
    plan=json.loads((ROOT/'analysis/localization/menu-v7-audit/gallery-title-13-glyph-plan.json').read_text(encoding='utf8'))
    folder=ROOT/'localization/image_assets/menu-ui-v7'
    rgba=folder/'gallery-title-rgba';output=folder/'gallery-title-replacements';output.mkdir(exist_ok=True)
    reports=[]
    for name in sorted({r['source_filename'] for r in plan['rows']}):
        rows=[r for r in plan['rows'] if r['source_filename']==name];original=(ROOT/rows[0]['source_DDS']).read_bytes()
        if sha(original)!=rows[0]['source_SHA256']:raise ValueError('Original source SHA changed')
        h,w=struct.unpack_from('<II',original,12);stem=Path(name).stem;pixels=(rgba/(stem+'.rgba')).read_bytes()
        if len(pixels)!=w*h*4:raise ValueError('GDI output dimensions differ')
        new=bytearray(original);allowed=set();mask_proofs=[]
        for row in rows:
            x,y,rw,rh=row['UV_xywh'];mask=(rgba/(stem+f'.{x}-{y}.mask')).read_bytes()
            if len(mask)!=w*h:raise ValueError('Invalid touched-pixel mask')
            count=0
            for n,touched in enumerate(mask):
                if not touched:continue
                xx,yy=n%w,n//w
                if not(x<=xx<x+rw and y<=yy<y+rh):raise ValueError('GDI mask escapes exact TXOS cell')
                q=128+n*4;a=n*4;r,g,b,alpha=pixels[a:a+4]
                new[q:q+4]=bytes((b,g,r,alpha));allowed.update(range(q,q+4));count+=1
            if not count:raise ValueError('Empty glyph mask')
            mask_proofs.append(dict(order=row['order'],JP=row['JP'],KO=row['KO_draft'],TXOS_index=row['TXOS_index'],UV_xywh=row['UV_xywh'],touched_pixels=count,capsule=row['colored_capsule_background_present']))
        changed=validate(original,new,allowed)
        # Every untouched pixel, including capsule color/background, retains
        # original four DDS bytes rather than a GDI roundtrip approximation.
        target=output/name;target.write_bytes(new)
        decoded=[(new[n+2],new[n+1],new[n],new[n+3]) for n in range(128,len(new),4)]
        png(target.with_suffix('.png'),decoded,w,h)
        tests=[]
        bad=bytearray(new);bad[12]^=1
        try:validate(original,bad,allowed)
        except ValueError:tests.append('header mutation rejected')
        else:raise AssertionError('header guard failed')
        protected=next(n for n in range(128,len(new)) if n not in allowed)
        bad=bytearray(new);bad[protected]^=1
        try:validate(original,bad,allowed)
        except ValueError:tests.append('protected pixel mutation rejected')
        else:raise AssertionError('outside mask guard failed')
        reports.append(dict(name=name,source_sha256=sha(original),replacement_sha256=sha(new),dimensions=[w,h],format='BGRA32',words=len(rows),changed_bytes=changed,outside_mask_bytes_changed=0,DDS_header_changed=0,all_capsule_background_pixels_outside_old_and_new_glyph_masks_byte_exact=True,English_digits_triangles_medals_and_other_cells_byte_exact=True,actual_source_copy_count=len(rows[0]['actual_active_copies']),mask_proofs=mask_proofs,negative_checks=tests))
    result=dict(status='PASS_OFFLINE_CANDIDATE',files=reports,words=13,source_alpha_only=True,source_alpha_cutoff=64,actual_alpha_bands=13,composition='GDI+',game_rendering_verified=False,active_modified=False,limitations=['Capsule background hidden beneath old JP glyph is reconstructed from same-capsule neighboring row donors; all other base pixels retain original DDS bytes.','Small-label readability reviewed in enlarged previews, not yet in-game.'])
    (output/'manifest.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print('PASS2 DDS /13 words /outside0 /4 negative checks')


if __name__=='__main__':main()
