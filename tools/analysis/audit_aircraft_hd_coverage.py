"""Audit global source-byte replacement coverage, including shared maps."""
import json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
    load=lambda p:json.loads((ROOT/p).read_text(encoding='utf8'))
    catalog=load('analysis/aircraft-texture-comparison/manifest.json')
    pack=load('native/assets/aircraft-hd-manifest.json')
    by_sha={e['source_sha256']:(i,e) for i,e in enumerate(pack['entries'])}
    models=[]
    for m in catalog['models']:
        matched=[];retained=[]
        for t in m['battle']['textures']:
            row=by_sha.get(t['sha256'])
            if row:
                i,e=row;matched.append(dict(map=i,source=t['name'],target=e['target'],kind=e['kind'],shared=m['key'] not in e['models']))
            else:retained.append(t['name'])
        models.append(dict(key=m['key'],names=m['names'],replacements=matched,retained_original=retained,
                           color_map_replaced=any(t['kind']!='normal' for t in matched),normal_map_replaced=any(t['kind']=='normal' for t in matched)))
    run=Path(load('experiments/vf0-hd-v1/latest-run.json')['run'])
    log=(run/'stderr.log').read_text(encoding='utf8',errors='replace');observed=[]
    for match in re.finditer(r'\[aircraft-hd\] matched map=(\d+) fmt=(\w+) (\d+) -> (\d+)',log):
        i=int(match[1]);e=pack['entries'][i];observed.append(dict(map=i,source=e['source'],target=e['target'],source_size=int(match[3]),target_size=int(match[4])))
    report=dict(models_scanned=len(models),models_with_any_replacement=sum(bool(m['replacements']) for m in models),
                models_with_color_replacement=sum(m['color_map_replaced'] for m in models),models_with_normal_replacement=sum(m['normal_map_replaced'] for m in models),
                unique_registered_maps=len(pack['entries']),all_existing_files_automatically_admitted=False,
                runtime_guards=['startup option enabled','compiled pack integrity pin','2D normalized non-cubemap BC1/BC2/BC3','registered source dimensions and format','complete first-mip byte match'],
                live_log=str(run/'stderr.log'),observed_swaps=observed,all_models_live_verified=False,models=models)
    (ROOT/'analysis/aircraft-hd-coverage.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('models','runtime_guards')},ensure_ascii=True))
if __name__=='__main__':main()
