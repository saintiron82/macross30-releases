"""Close the image gaps found by the Japanese/Korean/English route audit."""
import struct
import subprocess
from english_pack import ROOT, digest, write
from build_english_ui_images import BASE, TOOLS, read, invoke
from stage_gop_primary_mirrors import pidx, read_payload
from encode_misc_menu_replacements import pixels


if __name__=='__main__':
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
    assets=[];sheets=[];glyphs=[]
    definitions=[('placename_s00.dds','d9f18dee427b8a2212d8a5f01c44c38ea53b3316d803e0b3fbd8f62476723d85',
                  [('Gefion',[0,0,333,100]),('Hangar',[0,100,114,50]),('Bridge',[114,100,136,50])]),
                 ('quest_title_01_02.dds','7d328ba39c42e93bd75797e324b7ea7e8cfac4c2fe80f1f574172dfa1d6894fd',[('Bandits',[176,8,296,80])])]
    caution=read(ROOT/'localization/image_assets/caution-ui-v4/replacements/ope60_title.manifest.json')
    segments=['BAT','TLE','STA','RT','FIN','ISH','MIS','SION','OBJEC','TIVE','COM','PLETE','FAI','LED','CON','TINUE']
    definitions.append(('ope60_title.dds',caution['source_sha256'],[(text,r['source_cell_xywh']) for text,r in zip(segments,caution['mappings'])]))
    master=pidx(pack/'PACK.IDX')[1]
    for name,expected,labels in definitions:
        source=next(read_payload(pack/r['pack'].split('/')[-1].upper(),r)[0] for r in master if r['name']==name and digest(read_payload(pack/r['pack'].split('/')[-1].upper(),r)[0])==expected)
        h,w=struct.unpack_from('<II',source,12)
        path=BASE/'hud-input'/name;path.write_bytes(source)
        raw=bytes(v for r,g,b,a in pixels(source,w,h) for v in (b,g,r,a));bgra=path.with_suffix('.bgra');bgra.write_bytes(raw)
        rows=[]
        for text,roi in labels:
            n=len(glyphs)+1;sheet=BASE/'generated'/f'remaining-{n}.png'
            sheets.append(dict(path=str(sheet),width=1024,height=80,rows=[dict(top=10,bottom=70,text=text)]))
            glyphs.append(dict(glyph_id=n,sheet=str(sheet),row=0,row_count=1))
            rows.append(dict(glyph_id=n,source=text,target=text,roi_xywh=roi,style='silver_transparent'))
        assets.append(dict(name=name,source_DDS=str(path),source_sha256=expected,source_BGRA=str(bgra),
                           source_BGRA_sha256=digest(raw),width=w,height=h,format='BC3' if source[84:88]==b'DXT5' else 'BGRA8',
                           output_stem=path.stem,labels=rows,primary_records=[]))
    write(BASE/'remaining-plan.json',dict(assets=assets))
    write(BASE/'remaining-map.json',dict(rows=glyphs));write(BASE/'remaining-glyph-spec.json',dict(sheets=sheets))
    subprocess.run(['pwsh','-NoProfile','-File',str(TOOLS/'render_english_glyphs.ps1'),'-Spec',str(BASE/'remaining-glyph-spec.json')],check=True,cwd=ROOT)
    invoke('compose_misc_menu_labels.ps1',['-GlyphMap',str(BASE/'remaining-map.json'),'-Plan',str(BASE/'remaining-plan.json'),'-Output',str(BASE/'remaining-composited')])
    invoke('encode_misc_menu_replacements.py',['--composition',str(BASE/'remaining-composited/composition.json'),'--plan',str(BASE/'remaining-plan.json'),'--output',str(BASE/'remaining-replacements')])
