"""Build English quest titles and region banners using existing bounded compositors."""
import subprocess
import sys
from english_pack import ROOT, write
from build_english_ui_images import BASE, TOOLS, read, invoke


def main():
    labels=read(ROOT/'localization/en-US/images/region-labels.json')
    sheets=[]
    def sheet(filename,texts):
        path=BASE/'generated'/filename
        sheets.append(dict(path=str(path),width=1024,height=80*len(texts),
                           rows=[dict(top=i*80+10,bottom=i*80+70,text=labels[t]) for i,t in enumerate(texts)]))
        return path
    quest=read(ROOT/'localization/image_assets/caution-ui-v4/quest-title-glyph-plan.json')
    rows=[r for r in quest['ordered39'] if any(ord(c)>127 for c in r['JP'])]
    if len(rows)!=37:
        raise ValueError('Unexpected quest title scope')
    glyph=sheet('quest-37.png',[r['JP'] for r in rows])
    for i,r in enumerate(rows):
        r.update(KO=labels[r['JP']],glyph_source=str(glyph),glyph_source_cell_xywh=[0,i*80,1024,80])
    write(BASE/'quest-plan.json',dict(ready=True,rows=rows))
    bcf=read(ROOT/'analysis/localization/region-name-audit/b-c-f-s-definitive.json')
    for r in bcf['rows']:
        r['KO']=labels[r['JP']]
    write(BASE/'place-plan.json',bcf)
    place_sheet=sheet('place-21.png',[r['JP'] for r in bcf['rows']])
    region=read(ROOT/'localization/image_assets/region-names-v1/r-t-input/plan.json')
    unique={}
    for r in region['assets']:
        for label in r['labels']:
            label['target']=labels[label['source']]
            unique[label['glyph_id']]=label
    glyphs=list(unique.values())
    region_sheet=sheet('region-24.png',[r['source'] for r in glyphs])
    write(BASE/'region-plan.json',region)
    write(BASE/'region-map.json',dict(rows=[dict(glyph_id=r['glyph_id'],sheet=str(region_sheet),row=i,row_count=len(glyphs)) for i,r in enumerate(glyphs)]))
    write(BASE/'region-glyph-spec.json',dict(sheets=sheets))
    subprocess.run(['pwsh','-NoProfile','-File',str(TOOLS/'render_english_glyphs.ps1'),'-Spec',str(BASE/'region-glyph-spec.json')],check=True,cwd=ROOT)
    refs='System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Collections'
    change=[('-ReferencedAssemblies System.Drawing -TypeDefinition','-ReferencedAssemblies '+refs+' -TypeDefinition')]
    if '--region-only' in sys.argv:
        invoke('compose_misc_menu_labels.ps1',['-GlyphMap',str(BASE/'region-map.json'),'-Plan',str(BASE/'region-plan.json'),'-Output',str(BASE/'region-composited-v2')])
        invoke('encode_misc_menu_replacements.py',['--composition',str(BASE/'region-composited-v2/composition.json'),'--plan',str(BASE/'region-plan.json'),'--output',str(BASE/'region-replacements')])
        return
    (BASE/'quest-rgba').mkdir()
    invoke('render_quest_title_batch.ps1',['-Plan',str(BASE/'quest-plan.json'),'-Output',str(BASE/'quest-rgba')],change)
    invoke('build_quest_title_batch.py',['--plan',str(BASE/'quest-plan.json'),'--rgba',str(BASE/'quest-rgba'),'--output',str(BASE/'quest-replacements')])
    invoke('build_place_bcf_v7.ps1',['-Plan',str(BASE/'place-plan.json'),'-Glyph',str(place_sheet),'-Output',str(BASE/'place-bcf-rgba')],change)
    invoke('pack_place_bcf_v7.py',substitutions=[('analysis/localization/region-name-audit/b-c-f-s-definitive.json', (BASE/'place-plan.json').relative_to(ROOT).as_posix()),('localization/image_assets/menu-ui-v7',BASE.relative_to(ROOT).as_posix())])
    invoke('compose_misc_menu_labels.ps1',['-GlyphMap',str(BASE/'region-map.json'),'-Plan',str(BASE/'region-plan.json'),'-Output',str(BASE/'region-composited')])
    invoke('encode_misc_menu_replacements.py',['--composition',str(BASE/'region-composited/composition.json'),'--plan',str(BASE/'region-plan.json'),'--output',str(BASE/'region-replacements')])


if __name__=='__main__':
    main()
