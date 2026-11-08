"""Apply source-guarded locale UI variants and measure actual BFNT advances."""
import argparse
import json
from pathlib import Path


def apply_variants(project, variants):
    if variants['schema_version'] != 1 or project['locale'] != variants['locale']:
        raise ValueError('Layout variant locale/schema mismatch')
    seen=set()
    for row in variants['entries']:
        key=row['id']
        if key in seen:raise ValueError('Duplicate layout variant: '+key)
        seen.add(key)
        original=project['entries'][key]
        if original['source']!=row['source_ja'] or original['source_sha256']!=row['source_sha256']:
            raise ValueError('Layout variant source changed: '+key)
        text=row['ru']
        if not text.strip() or '\0' in text or '\n' in text or '\\n' in text:
            raise ValueError('Single-line UI variant required: '+key)
        original['text']=text
        original['display_variant_note']=row['reason']
    return project


def measure(text, glyphs):
    cursor=0;left=0;right=0
    for c in text:
        g=glyphs[ord(c)]
        left=min(left,cursor+g['bearing_x'])
        right=max(right,cursor+g['bearing_x']+g['width'])
        cursor+=g['advance']
    return dict(advance=cursor,ink_left=left,ink_right=right,extent=max(cursor,right)-left)


def audit(variants,abf):
    from audit_runtime_font import read_font
    glyphs={g['codepoint']:g for g in read_font(abf)[1]}
    rows=[]
    for row in variants['entries']:
        before=measure(row['previous_ru'],glyphs);after=measure(row['ru'],glyphs)
        rows.append(dict(id=row['id'],area=row['area'],before=before,after=after,budget=row['budget_atlas_units'],fits=after['extent']<=row['budget_atlas_units'],text=row['ru']))
    return dict(all_fit=all(r['fits'] for r in rows),entries=rows,actual_game_visual_check=False)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--project',type=Path,required=True);p.add_argument('--variants',type=Path,required=True);p.add_argument('--font',type=Path,required=True);p.add_argument('--audit-output',type=Path,required=True);a=p.parse_args()
    project=json.loads(a.project.read_text(encoding='utf8'));variants=json.loads(a.variants.read_text(encoding='utf8'))
    result=audit(variants,a.font.read_bytes())
    if not result['all_fit']:raise ValueError('Layout variants exceed budgets: '+str([r for r in result['entries'] if not r['fits']]))
    apply_variants(project,variants)
    a.project.write_text(json.dumps(project,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    a.audit_output.parent.mkdir(parents=True,exist_ok=True);a.audit_output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print(json.dumps(dict(entries=len(result['entries']),all_fit=result['all_fit'])))
if __name__=='__main__':main()
