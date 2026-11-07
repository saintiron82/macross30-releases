"""Small self-contained original/Korean asset comparison; no game launch."""
import base64
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/battle-ui-v1'
ORIGINAL = ROOT/'native/analysis/battle-ui-localization'
PAIRS = [
    ('전투 메시지 26개', 'ope29', '@data'),
    ('전방 주의', 'front_attention', 'search2'),
    ('구조 안내', 'hud_rescue', 'search2'),
    ('퀘스트 보상', 'stay_tx_dialogue', ''),
    ('첫 퀘스트 제목', 'quest_title_01_01', ''),
    ('레이스 시작', 'start01', ''),
    ('레이스 도착', 'goal01', ''),
    ('지도 지명 14개', 'hud_mapname00', ''),
    ('지도 지명 28개', 'hud_mapname01', ''),
]


def image(path):
    return '<img src="data:image/png;base64,' + base64.b64encode(path.read_bytes()).decode('ascii') + '">'


html = '''<!doctype html><html lang="ko"><meta charset="utf-8"><title>전투 UI 한글화 검수</title>
<style>body{font:16px system-ui;background:#16191e;color:#eee;max-width:1200px;margin:24px auto;padding:12px}
section{border-top:1px solid #555;padding:12px 0}.pair{display:flex;gap:20px;flex-wrap:wrap}
figure{margin:0;max-width:48%}img{max-width:100%;height:auto;background:#242a32;display:block;margin:10px 0}
figcaption{color:#aaa}@media(max-width:750px){figure{max-width:100%}}
table{border-collapse:collapse}td,th{border:1px solid #666;padding:8px}</style>
<h1>전투·지도 UI 한글화</h1><p>실제 게임 캡처가 아닌 원본/교체 이미지 비교입니다. 아이콘·틀·영문과 원본 크기를 보존한 DDS 리소스입니다.</p>
<p>인물·NPC 이름은 72종, 139개 위치를 별도의 한글 ELF에 반영했습니다. 게임 안 표시 검수는 별도입니다.</p>
<table><tr><th>이름 예시</th><th>번역</th></tr><tr><td>アイシャ・ブランシェット</td><td>아이샤 블란셰트</td></tr>
<tr><td>リオン・榊</td><td>리온 사카키</td></tr><tr><td>ミーナ・フォルテ</td><td>미나 포르테</td></tr></table>
'''
for title, name, subfolder in PAIRS:
    source = (ROOT/'analysis/assets/data' if subfolder == '@data' else ORIGINAL/subfolder)/(name+'.png')
    replacement = BUNDLE/'replacements'/(name+'.png')
    html += f'<section><h2>{title}</h2><div class="pair"><figure><figcaption>일본어 원본</figcaption>{image(source)}</figure>'
    html += f'<figure><figcaption>한글 교체본</figcaption>{image(replacement)}</figure></div></section>'
html += '<p>원형 경고 UI가 참조하는 ope29의 실제 전투 문구 26개를 번역했습니다. 원문이 다른 문구를 戦闘開始로 임의 변경하지 않았습니다.</p></html>'
out = BUNDLE/'preview.html'
out.write_text(html, encoding='utf-8')
print(f'{out}: {out.stat().st_size:,} bytes')
