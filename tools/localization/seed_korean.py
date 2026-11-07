"""Seed a small reviewed-language draft; does not patch game files."""
import json
from pathlib import Path

translations = {
    '難易度選択': '난이도 선택',
    '※難易度はここでしか選択できません': '※난이도는 여기서만 선택할 수 있습니다',
    '○：決定　×：タイトルに戻る': '○: 확인　×: 타이틀로',
    '操作タイプ': '조작 방식',
    '※操作タイプは「オプション」でも変更可能です': '※조작 방식은 「옵션」에서도 변경할 수 있습니다',
    '○：決定　×：戻る': '○: 확인　×: 뒤로',
    'リトライ': '재도전',
    '少し前から再開': '조금 전부터 재개',
    '○：決定': '○: 확인',
    'ロード': '불러오기',
    'セーブデータから再開': '저장 데이터로 재개',
    'タイトルに戻る': '타이틀로 돌아가기',
}
path = Path('localization/ko-KR/gop_guide.json')
doc = json.loads(path.read_text(encoding='utf-8'))
count = 0
for row in doc['entries']:
    if row['source'] in translations and not row['translation']:
        row['translation'] = translations[row['source']]
        row['status'] = 'draft; font/layout/in-game verification pending'
        count += 1
path.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding='utf-8')
print('Draft Korean entries added:', count)
