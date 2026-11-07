"""Build a small offline original/replacement asset preview, not game screenshots."""
import base64
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'localization/image_assets/ui-ko-v1/preview.html'
PAIRS = [
    ('메뉴 글자', 'native/analysis/quest-ui-localization/hud_menu_text00.png',
     'localization/image_assets/menu-labels-v2/replacements/hud_menu_text00.png'),
    ('목표 지점', 'native/analysis/quest-ui-localization/hud_maptext_00.png',
     'localization/image_assets/menu-labels-v2/replacements/hud_maptext_00.png'),
    ('지역명 3종', 'analysis/assets/data/hng_field_name.png',
     'localization/image_assets/field-names-v1/replacements/hng_field_name.png'),
    ('조작 안내 TYPE A', 'localization/image_assets/hud-guide-v1/hud_cguide_a.original.png',
     'localization/image_assets/hud-guide-v1/hud_cguide_a.ko-draft.png'),
    ('조작 안내 TYPE B', 'localization/image_assets/hud-guide-v1/hud_cguide_b.original.png',
     'localization/image_assets/hud-guide-v1/hud_cguide_b.ko-draft.png'),
    ('조작 안내 TYPE C', 'localization/image_assets/hud-guide-v1/hud_cguide_c.original.png',
     'localization/image_assets/hud-guide-v1/hud_cguide_c.ko-draft.png'),
]


def img(path):
    data = base64.b64encode((ROOT/path).read_bytes()).decode('ascii')
    return f'<img src="data:image/png;base64,{data}">'


html = '''<!doctype html><html lang="ko"><meta charset="utf-8"><title>Macross UI 한글 리소스</title>
<style>body{background:#171a20;color:#eee;font:16px system-ui;max-width:1200px;margin:30px auto;padding:12px}
section{border-top:1px solid #555;padding:14px 0} .pair{display:flex;gap:22px;flex-wrap:wrap}
figure{margin:0}img{display:block;width: min(520px,90vw);height:auto;background:#313641;margin:10px 0}
figcaption{color:#aaa} table{border-collapse:collapse}td,th{padding:8px;border:1px solid #555}</style>
<h1>화면 이미지 한글화</h1><p>원본과 교체용 리소스 비교입니다. 실제 게임 캡처가 아닙니다. 게임 안 가독성·배치는 확인 전입니다.</p>
<p>DDS 헤더·원본 크기·교체 영역 밖 픽셀을 유지합니다. 설치 시 원본을 백업하며 실행 중에는 설치하지 않습니다.</p>
'''
for title, original, replacement in PAIRS:
    html += f'<section><h2>{title}</h2><div class="pair"><figure><figcaption>일본어 원본</figcaption>{img(original)}</figure>'
    html += f'<figure><figcaption>한글 교체본</figcaption>{img(replacement)}</figure></div></section>'
html += '<section><h2>실행 파일의 UI 문자열</h2><table><tr><th>원문</th><th>번역</th></tr>'
for original, translated in [('クエスト詳細', '퀘스트 상세'), ('クリア条件', '클리어조건'), ('報酬', '보상')]:
    html += f'<tr><td>{original}</td><td>{translated}</td></tr>'
html += '</table></section></html>'
OUT.write_text(html, encoding='utf-8')
print(f'{OUT}: {OUT.stat().st_size:,} bytes')
