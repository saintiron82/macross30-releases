"""Audit English coverage, original-font repertoire and protected control tokens."""
import json
import re
from english_pack import ROOT,write
from english_font import original_font
from gop_strings import TOKEN

if __name__=='__main__':
    _,_,glyphs,_=original_font();base=ROOT/'localization/en-US';missing=[];untranslated=[];language=[];tokens=[];count=0
    for path in sorted((base/'structured').glob('*.json')):
        doc=json.loads(path.read_text(encoding='utf8'))
        for row in doc['entries']:
            text=row['translation']
            if not text:untranslated.append(row['id']);continue
            count+=1
            if TOKEN.findall(text)!=TOKEN.findall(row['source']):tokens.append(row['id'])
            visible=TOKEN.sub('',text)
            if re.search(r'[\u3040-\u30ff\u3400-\u9fff\uac00-\ud7af]',visible):language.append(row['id'])
            absent=sorted({ord(c) for c in visible if not c.isspace() and ord(c) not in glyphs})
            if absent:missing.append(dict(id=row['id'],codepoints=[hex(c) for c in absent],translation=text))
    report=dict(translated=count,untranslated=untranslated,non_english=language,token_mismatches=tokens,missing_original_font_glyphs=missing,semantic_review_complete=False)
    write(base/'build/catalog-audit.json',report)
    print(json.dumps(dict(translated=count,untranslated=len(untranslated),non_english=len(language),token_mismatches=len(tokens),missing_font=len(missing))))
    if missing or language or tokens:raise ValueError('English catalog validation failed; see catalog-audit.json')
