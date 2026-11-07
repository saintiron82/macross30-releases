"""Reuse existing reviewed HUD sprite regions with English glyphs."""
from collections import defaultdict
import struct
import subprocess
from english_pack import ROOT, digest, write
from build_english_ui_images import BASE, TOOLS, read, invoke
from stage_gop_primary_mirrors import pidx, read_payload
from encode_misc_menu_replacements import pixels


def main():
    labels={}
    for name in ('labels','region-labels','hud-labels'):
        labels.update(read(ROOT/f'localization/en-US/images/{name}.json'))
    regions=defaultdict(list)
    expected={}
    def add(name,text,roi,style='silver_transparent'):
        if text not in labels:
            raise ValueError('Missing English HUD label: '+text)
        row=dict(source=text,target=labels[text],roi_xywh=roi,style=style)
        if row not in regions[name]:regions[name].append(row)
    home=ROOT/'localization/image_assets'
    for bundle in ('quest-ui-v1','menu-labels-v1','menu-labels-v2','field-names-v1'):
        for asset in read(home/bundle/'replacements/manifest.json')['results']:
            expected.setdefault(asset['name'],asset['source_sha256'])
            for row in asset['labels']:
                if asset['name']=='hud_maptext_00.dds' and row['label']=='yuria_islands':continue
                add(asset['name'],row['label'],row['allowed_roi'])
    for manifest in ('map-labels-manifest','combat-labels-manifest'):
        for asset in read(home/f'battle-ui-v1/replacements/{manifest}.json')['results']:
            expected.setdefault(asset['name'],asset['source_sha256'])
            for row in asset['labels']:
                add(asset['name'],row['Japanese'],row['allowed_text_ROI'])
    for asset in read(home/'caution-ui-v4/replacements/weapon-labels-manifest.json')['results']:
        expected.setdefault(asset['name'],asset['source_sha256'])
        for row in asset['labels']:
            add(asset['name'],row['Japanese'],row['allowed_text_ROI'])
    four=['スタート','ゴール','クエスト報酬','ゼントラディの少女']
    for text,asset in zip(four,read(home/'battle-ui-v1/replacements/four-labels-manifest.json')['results']):
        expected.setdefault(asset['name'],asset['source_sha256'])
        add(asset['name'],text,asset['allowed_roi'])
    for text,asset in zip(['前方注意','救援要請'],read(home/'battle-ui-v1/replacements/warning-labels-manifest.json')['results']):
        expected.setdefault(asset['name'],asset['source_sha256'])
        add(asset['name'],text,asset['allowed_roi'],'cyan_panel' if 'rescue' in asset['name'] else 'silver_transparent')
    guide=read(home/'hud-guide-v1/manifest.json')
    for asset in guide['assets']:
        expected.setdefault(asset['name'],asset['source_sha256'])
        for row in guide['labels']:
            add(asset['name'],row['source'],row['edit_rect'],'cyan_panel')
    for text,roi in [('通常',[1168,487,99,18]),('集合',[1168,527,99,20]),('スキル',[1168,567,99,19]),('僚機',[1159,641,39,23]),('特殊',[858,358,25,52])]:
        add('hud_cockpit_01.dds',text,roi)
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK'
    _,master,_=pidx(pack/'PACK.IDX')
    sources={}
    expected['hud_cockpit_01.dds']='9ed1c3cd54185b6c089c3cfb96ff48aaf5355b3845648e4e1446ede9f2a1d8a1'
    for row in master:
        if row['name'] not in regions:continue
        data,_=read_payload(pack/row['pack'].split('/')[-1].upper(),row)
        if digest(data)!=expected[row['name']]:continue
        if row['name'] in sources and sources[row['name']]!=data:
            raise ValueError('Multiple original HUD variants need review: '+row['name'])
        sources[row['name']]=data
    if set(sources)!=set(regions):raise ValueError('Missing HUD originals: '+str(set(regions)-set(sources)))
    folder=BASE/'hud-input';folder.mkdir(exist_ok=True)
    assets=[];glyphs=[];sheets=[]
    for name,rows in regions.items():
        data=sources[name];h,w=struct.unpack_from('<II',data,12)
        source=folder/name;source.write_bytes(data)
        rgba=pixels(data,w,h)
        bgra=bytes(v for r,g,b,a in rgba for v in (b,g,r,a))
        decoded=source.with_suffix('.bgra');decoded.write_bytes(bgra)
        for row in rows:
            n=len(glyphs)+1;row['glyph_id']=n
            sheet=BASE/'generated'/f'hud-{n}.png'
            glyphs.append(dict(glyph_id=n,sheet=str(sheet),row=0,row_count=1))
            sheets.append(dict(path=str(sheet),width=1024,height=80,rows=[dict(top=10,bottom=70,text=row['target'])]))
        assets.append(dict(name=name,source_DDS=str(source),source_sha256=digest(data),
                           source_BGRA=str(decoded),source_BGRA_sha256=digest(bgra),
                           width=w,height=h,format='BC3' if data[84:88]==b'DXT5' else 'BGRA8',
                           output_stem=source.stem,labels=rows,
                           primary_records=[r for r in master if r['name']==name]))
    write(BASE/'hud-plan.json',dict(assets=assets))
    write(BASE/'hud-map.json',dict(rows=glyphs))
    write(BASE/'hud-glyph-spec.json',dict(sheets=sheets))
    subprocess.run(['pwsh','-NoProfile','-File',str(TOOLS/'render_english_glyphs.ps1'),'-Spec',str(BASE/'hud-glyph-spec.json')],check=True,cwd=ROOT)
    invoke('compose_misc_menu_labels.ps1',['-GlyphMap',str(BASE/'hud-map.json'),'-Plan',str(BASE/'hud-plan.json'),'-Output',str(BASE/'hud-composited-v2')])
    invoke('encode_misc_menu_replacements.py',['--composition',str(BASE/'hud-composited-v2/composition.json'),'--plan',str(BASE/'hud-plan.json'),'--output',str(BASE/'hud-replacements')])


if __name__=='__main__':main()
