"""Measure static ABF cursor/ink width; never rewrite captions or claim live fit."""
import hashlib
import json
from pathlib import Path
import struct
from audit_runtime_font import chunks,read_font

ROOT=Path(__file__).resolve().parents[2]


def main():
    bundle=ROOT/'localization/image_assets/battle-voice-font-v13'
    authority=json.loads((bundle/'candidate/authority.json').read_text(encoding='utf8'))
    entry=next(r for r in authority['entries'] if r['type']=='ABF')
    raw=Path(entry['replacement']).read_bytes()
    assert hashlib.sha256(raw).hexdigest()==entry['replacement_sha256']
    _,records,_=read_font(raw);glyphs={r['codepoint']:r for r in records}
    detail=next(payload for _,magic,kind,payload,_ in chunks(raw) if magic==b'BFNT' and kind==b'HDET')
    height=struct.unpack_from('>H',detail,2)[0];assert height==24
    scale=28/height
    source=ROOT/'native/macross30/battle_voice_catalog.inc'
    rows=[];over=[];review=[];multiline=[]
    for line in source.read_text(encoding='utf8').splitlines():
        if not line.lstrip().startswith('{'):continue
        value=line.strip().removesuffix(',')
        stem,text=json.loads('['+value[1:-1]+']')
        lines=text.replace('#n','\n').replace('\\n','\n').split('\n')
        widths=[]
        for part in lines:
            cursor=0;left=0;right=0
            for char in part:
                g=glyphs[ord(char)]
                left=min(left,cursor+g['bearing_x'])
                right=max(right,cursor+g['bearing_x']+g['width'])
                cursor+=g['advance']
            widths.append(dict(cursor_px=round(cursor*scale,3),ink_width_px=round((right-left)*scale,3)))
        maximum=max(max(w.values()) for w in widths)
        row=dict(stem=stem,text=text,line_count=len(lines),maximum_width_px=maximum,lines=widths)
        rows.append(row)
        if maximum>720:over.append(row)
        if maximum>680:review.append(row)
        if len(lines)>1:multiline.append(stem)
    report=dict(status='STATIC_RISK_AUDIT_NOT_LIVE_LAYOUT_VERIFICATION',entries=len(rows),
        font_sha256=entry['replacement_sha256'],catalog_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        nominal_pane_width_px=720,caption_height_px=28,font_native_height=height,scale=scale,
        over_720=len(over),over_review_budget_680=len(review),multiline_entries=len(multiline),
        longest=sorted(rows,key=lambda r:r['maximum_width_px'],reverse=True)[:10],
        overflow_risks=over,review_risks=review,multiline_stems=multiline,
        limitations=['Static advance/ink sum; runtime kerning, final transformed rectangle and vertical clipping need live verification.',
                    'Existing native formatting controls other than #n/backslash-n have not been interpreted.',
                    'No line breaks, translations, font sizes or timing were changed.'])
    out=ROOT/'analysis/localization/battle-caption-width-risk-v1.json'
    out.write_bytes((json.dumps(report,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
    print(json.dumps({k:report[k] for k in ['status','entries','over_720','over_review_budget_680','multiline_entries']}))


if __name__=='__main__':main()
