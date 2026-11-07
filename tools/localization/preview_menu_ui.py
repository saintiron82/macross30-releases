"""Offline comparison for the remaining my-ship menu and chapter title."""
import base64
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/menu-ui-v2'


def image(path):
    return '<img src="data:image/png;base64,' + base64.b64encode(path.read_bytes()).decode('ascii') + '">'


html = '''<!doctype html><html lang="ko"><meta charset="utf-8"><title>누락 메뉴 한글화</title>
<style>body{background:#16191e;color:#eee;font:16px system-ui;max-width:1200px;margin:24px auto;padding:14px}
.pair{display:flex;gap:20px;flex-wrap:wrap}figure{margin:0;max-width:48%}img{max-width:100%;height:auto;background:#242a32}
td,th{padding:8px;border:1px solid #666}table{border-collapse:collapse}section{border-top:1px solid #555;padding:16px 0}</style>
<h1>누락 메뉴·제목 한글화</h1><p>게임 캡처가 아닌 실제 교체용 이미지와 동적 문자열 검수표입니다.</p>
<table><tr><th>원문</th><th>교체</th></tr>'''
doc = json.loads((BUNDLE/'EBOOT.menu.manifest.json').read_text(encoding='utf-8'))
labels = {r['source']: r['translation'] for r in doc['strings']}
for source in ['マイシップ', 'セーブ・ロード', 'オプション', '出撃']:
    html += f'<tr><td>{source}</td><td>{labels[source]}</td></tr>'
html += '</table>'
pairs = [('밴디트 제목', ROOT/'native/analysis/quest-title-sources/quest_title_01_02.png',
          BUNDLE/'replacements/quest_title_01_02.png'),
         ('게피온·격납고·브리지', ROOT/'native/analysis/myship-ui-localization/placename_s00-black.png',
          BUNDLE/'replacements/placename_s00-alpha-on-black.png')]
for title, original, replacement in pairs:
    html += f'<section><h2>{title}</h2><div class="pair"><figure><figcaption>일본어 원본</figcaption>{image(original)}</figure>'
    html += f'<figure><figcaption>한글 교체본</figcaption>{image(replacement)}</figure></div></section>'
html += '<p>기존 이름·전투·지도 번역을 유지합니다. 실제 설치 상태는 ready.json과 installation.json을 참조합니다.</p></html>'
out = BUNDLE/'preview.html'
out.write_text(html, encoding='utf-8')
print(f'{out}: {out.stat().st_size:,} bytes')
