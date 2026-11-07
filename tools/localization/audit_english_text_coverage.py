"""Verify installed text against catalogs and list every remaining Japanese entry."""
import csv
import json
import re
import argparse
from pathlib import Path
from english_pack import ROOT,digest,write
from gop_strings import parse
from stage_gop_primary_mirrors import pidx,read_payload,fsts_entries

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--manifest',type=Path)
    args=parser.parse_args()
    active=dict(manifest=str(args.manifest.resolve())) if args.manifest else json.loads((ROOT/'localization/en-US/preview-active.json').read_text(encoding='utf8'))
    preview=Path(active['manifest']).parent
    pack=preview/'PS3_GAME/USRDIR/DATA/PACK'
    _,primary,archives=pidx(pack/'PACK.IDX')
    paths={p.name.lower():p for p in pack.iterdir()}
    def payload(row):return read_payload(paths[Path(row['pack']).name.lower()],row)[0]
    nested={Path(r['name']).name:r['payload'] for r in fsts_entries(payload(next(r for r in archives if r['name']=='gop')))}
    japanese=re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')
    groups=[];pending=[];translated_with_japanese=[]
    for path in sorted((ROOT/'localization/en-US/structured').glob('*.json')):
        doc=json.loads(path.read_text(encoding='utf8'))
        name=Path(doc['source_file']).name
        expected=(ROOT/'localization/en-US/build/gop'/name).read_bytes()
        routes=[r for r in primary if r['name']==name]
        assert routes and all(payload(r)==expected for r in routes),name+' primary readback mismatch'
        assert nested[name]==expected,name+' secondary readback mismatch'
        actual={r['index']:r['text'] for r in parse(expected)}
        count=0
        for entry in doc['entries']:
            target=entry['translation'] or entry['source']
            assert actual[entry['index']]==target,entry['id']
            if entry['translation']:
                count+=1
                if japanese.search(target):translated_with_japanese.append(entry['id'])
            else:pending.append(dict(catalog=path.stem,id=entry['id'],index=entry['index'],source_ja=entry['source'],installed_text=target,japanese_remaining=bool(japanese.search(target))))
        groups.append(dict(catalog=path.stem,total=len(doc['entries']),english=count,remaining=len(doc['entries'])-count,primary_routes=len(routes),secondary_verified=True,sha256=digest(expected)))
    report=dict(preview=str(preview),catalogs=groups,total=sum(r['total'] for r in groups),english=sum(r['english'] for r in groups),remaining=len(pending),translated_with_japanese=translated_with_japanese,pending=pending,visual_review='pending')
    write(ROOT/'localization/en-US/build/installed-text-coverage.json',report)
    with (ROOT/'localization/en-US/build/remaining-japanese.csv').open('w',encoding='utf-8-sig',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=['catalog','id','index','source_ja','installed_text','japanese_remaining']);writer.writeheader();writer.writerows(pending)
    md=['# 영어 텍스트 적용 관리표','',f'미리보기: `{preview.name}`. 실제 팩의 원본 참조와 중첩 참조를 모두 읽어 검증했다.',
        '',f'구조화 텍스트 {report["total"]:,}개 중 영어 {report["english"]:,}개, 미번역 {report["remaining"]:,}개. 화면 검수는 별도다.',
        '', '| 파일 | 전체 | 영어 적용 | 미번역 |', '|---|---:|---:|---:|']
    md += [f'| {r["catalog"]} | {r["total"]:,} | {r["english"]:,} | {r["remaining"]:,} |' for r in groups]
    md += ['', '[미번역 전체 CSV](../localization/en-US/build/remaining-japanese.csv) · [실제 적용 검증 JSON](../localization/en-US/build/installed-text-coverage.json)',
           '', '이 표는 GOP 문자열 범위다. 이미지, ELF 문구, 영상 SRT, 별도 음성 자막은 각 관리 대상에 포함해야 하며 이 수치만으로 전체 영문화를 선언하지 않는다.']
    (ROOT/'docs/localization-text-register.md').write_text('\n'.join(md)+'\n',encoding='utf8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('catalogs','pending')},ensure_ascii=False))
