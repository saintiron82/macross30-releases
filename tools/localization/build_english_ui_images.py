"""Reuse bounded Korean sprite compositors with English glyphs and isolated outputs."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
from english_pack import ROOT, write

BASE = ROOT / 'localization/en-US/images/ui-v1'
TOOLS = ROOT / 'tools/localization'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def invoke(name, arguments=(), substitutions=()):
    source = (TOOLS / name).read_text(encoding='utf8')
    for old, new in substitutions:
        if old not in source:
            raise ValueError(f'Missing compositor substitution: {name}: {old}')
        source = source.replace(old, new)
    # Keep sibling imports available for isolated copies of the existing codecs.
    if name.endswith('.py'):
        source = source.replace('Path(__file__).resolve().parents[2]', 'Path(' + repr(str(ROOT)) + ')')
        source = source.replace("from pathlib import Path", "from pathlib import Path\nimport sys\nsys.path.insert(0," + repr(str(TOOLS)) + ")", 1)
    script = BASE / name
    script.write_text(source, encoding='utf8')
    command = ['pwsh', '-NoProfile', '-File'] if name.endswith('.ps1') else [sys.executable]
    subprocess.run(command + [str(script)] + list(arguments), cwd=ROOT, check=True)


def main():
    if BASE.exists() and '--resume' not in sys.argv:
        raise FileExistsError('Fresh English image build required')
    for folder in ('generated', 'audit', 'replacements', 'misc-input'):
        (BASE / folder).mkdir(parents=True, exist_ok=True)
    labels = read(ROOT / 'localization/en-US/images/labels.json')
    sheets = []

    def sheet(filename, texts, bands=None, height=None):
        if bands is None:
            bands = [(i*80+10, i*80+70) for i in range(len(texts))]
            height = len(texts)*80
        path = BASE / 'generated' / filename
        sheets.append(dict(path=str(path), width=1024, height=height,
                           rows=[dict(top=a, bottom=b, text=labels[t]) for t, (a,b) in zip(texts,bands)]))
        return path

    misc = read(ROOT / 'localization/image_assets/menu-ui-v7/misc-input/plan.json')
    for row in misc['glyph_rows']:
        row['target'] = labels[row['source']]
    for asset in misc['assets']:
        for row in asset['labels']:
            row['target'] = labels[row['source']]
    write(BASE / 'misc-input/plan.json', misc)
    misc_sheet = sheet('misc-35.png', [r['source'] for r in misc['glyph_rows']])
    write(BASE / 'misc-map.json', dict(rows=[dict(glyph_id=r['glyph_id'], sheet=str(misc_sheet), row=i,
                                                row_count=35) for i,r in enumerate(misc['glyph_rows'])]))
    audit = ROOT / 'localization/image_assets/menu-ui-v7/audit'
    for name in ('controller27-final-rois.json', 'options6-final-rois.json', 'opt_logo00.bgra', 'opt_brighttext.bgra'):
        shutil.copyfile(audit / name, BASE / 'audit' / name)
    controller = read(audit / 'controller27-final-rois.json')
    bands = [(31,74),(89,130),(145,188),(201,244),(258,301),(316,358),(370,413),(427,468),(481,524),(539,580),(594,637),(652,694),(708,750),(765,808),(821,864),(878,921),(935,978),(992,1035),(1049,1092),(1106,1149),(1162,1206),(1220,1262),(1277,1319),(1334,1376),(1390,1432),(1441,1482),(1487,1529)]
    sheet('controller-27.png', [r['source'] for r in controller['labels']], bands, 1536)
    options = [r['source'] for r in read(audit/'options6-final-rois.json')['labels']]
    options += ['オプション','うっすらとS.M.Sマークが見える設定が推奨明度になります。','ご使用のモニターの見え方に合わせて調整してください。']
    sheet('options-9.png', options, [(71,175),(243,348),(422,529),(600,705),(788,895),(971,1074),(1152,1255),(1331,1372),(1431,1471)],1536)
    menu = read(ROOT/'native/analysis/develop-ui-localization/menu01-final-rois.json')
    menu_sheet = sheet('hangar-menu01-24.png',[r['Japanese'] for r in menu['labels']])
    other = read(ROOT/'localization/image_assets/data-ui-v6/other-final-rois.json')
    by_row = {r['row']:r['Japanese'] for r in other['labels']}
    sheet('hangar-other-17.png',[by_row[i] for i in range(17)])
    write(BASE/'other-final-rois.json',other)
    gallery=read(ROOT/'analysis/localization/menu-v7-audit/gallery-title-13-glyph-plan.json')
    for row in gallery['rows']:
        row['KO_draft']=labels[row['JP']]
    write(BASE/'gallery-plan.json',gallery)
    gallery_sheet=sheet('gallery-13.png',[r['JP'] for r in gallery['rows']])
    write(BASE/'glyph-spec.json',dict(sheets=sheets))
    subprocess.run(['pwsh','-NoProfile','-File',str(TOOLS/'render_english_glyphs.ps1'),'-Spec',str(BASE/'glyph-spec.json')],check=True,cwd=ROOT)
    relative=BASE.relative_to(ROOT).as_posix()
    old='localization/image_assets/menu-ui-v7'
    invoke('build_options_ui.ps1',['-Root',str(ROOT)],substitutions=[(old,relative)])
    invoke('build_options_extra.ps1',['-Root',str(ROOT)],substitutions=[(old,relative)])
    invoke('encode_options_ui.py',substitutions=[(old,relative)])
    invoke('build_hangar_menu01.ps1',['-GlyphSheet',str(menu_sheet),'-OutputDirectory',str(BASE/'replacements')])
    invoke('build_hangar_other.ps1',substitutions=[('localization/image_assets/data-ui-v6',relative)])
    invoke('compose_misc_menu_labels.ps1',['-GlyphMap',str(BASE/'misc-map.json'),'-Plan',str(BASE/'misc-input/plan.json'),'-Output',str(BASE/'misc-composited')])
    invoke('encode_misc_menu_replacements.py',['--composition',str(BASE/'misc-composited/composition.json'),'--plan',str(BASE/'misc-input/plan.json'),'--output',str(BASE/'misc-replacements')])
    refs='System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core,System.Collections'
    invoke('build_gallery_title_v7.ps1',['-Plan',str(BASE/'gallery-plan.json'),'-Glyph',str(gallery_sheet),'-Output',str(BASE/'gallery-title-rgba')],substitutions=[('-ReferencedAssemblies System.Drawing -TypeDefinition','-ReferencedAssemblies '+refs+' -TypeDefinition')])
    invoke('pack_gallery_title_v7.py',substitutions=[('analysis/localization/menu-v7-audit/gallery-title-13-glyph-plan.json',relative+'/gallery-plan.json'),(old,relative)])
    print('English option, controller, hangar, mission, item, gallery and difficulty image candidates built.')


if __name__ == '__main__':
    main()
