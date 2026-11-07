"""Compare actual Korean image changes against Japanese and English resource routes."""
from pathlib import Path
import json
import csv
import struct
import argparse
from english_pack import ROOT, digest, write
from stage_gop_primary_mirrors import pidx, read_payload
from stage_nested_ui import fsts_entries, decoded,region


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--manifest',type=Path);args=parser.parse_args()
    active=dict(manifest=str(args.manifest.resolve())) if args.manifest else json.loads((ROOT/'localization/en-US/preview-active.json').read_text(encoding='utf8'))
    english=Path(active['manifest']).parent/'PS3_GAME/USRDIR/DATA/PACK'
    roots=[ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK',ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK',english]
    catalogs=[pidx(root/'PACK.IDX') for root in roots]
    paths=[{p.name.lower():p for p in root.iterdir()} for root in roots]
    changed=[]
    assets={}
    registry_root=ROOT/'localization/image-registry'
    def blob(n,row):return read_payload(paths[n][Path(row['pack']).name.lower()],row)[0]
    def compare(name,jp,ko,en,route,scope='korean_baseline',extra=None):
        if jp==ko and scope=='korean_baseline':return
        original_sha,korean_sha,english_sha=map(digest,(jp,ko,en))
        identity=Path(name).stem+'-'+original_sha[:12]
        if identity not in assets:
            folder=registry_root/'assets'/identity
            folder.mkdir(parents=True,exist_ok=True)
            for locale,data in (('ja-JP',jp),('ko-KR',ko),('en-US',en)):
                (folder/(locale+'.dds')).write_bytes(data)
            height,width=struct.unpack_from('<II',jp,12)
            assets[identity]=dict(id=identity,name=Path(name).name,width=width,height=height,
                original_sha256=original_sha,korean_sha256=korean_sha,english_sha256=english_sha,
                source_files={locale:(folder/(locale+'.dds')).relative_to(ROOT).as_posix() for locale in ('ja-JP','ko-KR','en-US')},
                category='font' if name.startswith('data/font/') else 'ui_image',
                english_status='installed_needs_visual_review' if en!=jp and en!=ko else 'not_replaced',
                visual_review='pending',scope=scope,reported_text=extra,routes=[])
        asset=assets[identity]
        if asset['korean_sha256']!=korean_sha or asset['english_sha256']!=english_sha:
            raise ValueError('Same original image has inconsistent installed locale variants: '+identity)
        asset['routes'].append(dict(resource_path=name,**route))
        if scope=='korean_baseline':
            changed.append(dict(name=name,route=route,original_sha256=digest(jp),korean_sha256=digest(ko),
                                english_sha256=digest(en),english_replaced=en!=jp and en!=ko))
    by_primary=[{r['record']:r for r in cat[1]} for cat in catalogs]
    for row in catalogs[0][1]:
        if not row['name'].lower().endswith('.dds'):continue
        kr=by_primary[1][row['record']];en=by_primary[2][row['record']]
        fields=('pack','offset','size','compressed_size')
        if all(row[k]==kr[k] for k in fields):continue
        compare(row['name'],blob(0,row),blob(1,kr),blob(2,en),dict(kind='primary',record=row['record']))
    by_archive=[{r['record']:r for r in cat[2]} for cat in catalogs]
    for row in catalogs[0][2]:
        kr=by_archive[1].get(row['record']);en=by_archive[2].get(row['record'])
        if kr is None or en is None:raise ValueError('Missing archive route')
        if (row['offset'],row['size'])==(kr['offset'],kr['size']):continue
        original,korean,english=blob(0,row),blob(1,kr),blob(2,en)
        if original[:4]!=b'FSTS':continue
        maps=[{e['path']:e for e in fsts_entries(b)} for b in (original,korean,english)]
        for entry in maps[0].values():
            if not entry['path'].lower().endswith('.dds'):continue
            jp=decoded(original,entry)[0];ko=decoded(korean,maps[1][entry['path']])[0]
            if jp==ko:continue
            actual=decoded(english,maps[2][entry['path']])[0]
            compare(entry['path'],jp,ko,actual,dict(kind='nested',archive=row['name'],archive_record=row['record'],entry_index=entry['index']))
    additional=ROOT/'tools/localization/catalogs/additional_image_scope.json'
    if additional.exists():
        requested_rows=json.loads(additional.read_text(encoding='utf8'))['assets']
        for requested in requested_rows:
            record=requested['primary_record']
            jp,ko,en=[blob(n,by_primary[n][record]) for n in range(3)]
            if digest(jp)!=requested['original_sha256']:raise ValueError('Additional image source changed')
            compare(requested['name'],jp,ko,en,dict(kind='primary',record=record),scope='additional_original',extra=requested)
        requested_by_key={(r['name'],r['original_sha256']):r for r in requested_rows}
        names={r['name'] for r in requested_rows}
        for arc in catalogs[0][2]:
            path=paths[0][Path(arc['pack']).name.lower()]
            head=region(path,arc['offset'],20)
            if head[:4]!=b'FSTS':continue
            _,_,names_at,names_size=struct.unpack_from('<4I',head,4)
            table_names=region(path,arc['offset']+names_at,names_size)
            if not any(n.encode() in table_names for n in names):continue
            original=blob(0,arc)
            original_rows=fsts_entries(original)
            candidates=[e for e in original_rows if Path(e['path']).name in names]
            if not candidates:continue
            korean=blob(1,by_archive[1][arc['record']]);english=blob(2,by_archive[2][arc['record']])
            km={e['path']:e for e in fsts_entries(korean)};em={e['path']:e for e in fsts_entries(english)}
            for entry in candidates:
                jp=decoded(original,entry)[0];requested=requested_by_key.get((Path(entry['path']).name,digest(jp)))
                if requested is None:raise ValueError('Unknown nested additional image variant')
                compare(entry['path'],jp,decoded(korean,km[entry['path']])[0],decoded(english,em[entry['path']])[0],dict(kind='nested',archive=arc['name'],archive_record=arc['record'],entry_index=entry['index']),scope='additional_original',extra=requested)
    font=[r for r in changed if r['name'].startswith('data/font/')]
    ui=[r for r in changed if r not in font]
    missing=[r for r in ui if not r['english_replaced']]
    registry_path=ROOT/'tools/localization/catalogs/image_registry.json'
    previous=json.loads(registry_path.read_text(encoding='utf8')) if registry_path.exists() else {}
    old={r['id']:r for r in previous.get('assets',[])}
    for asset in assets.values():
        prior=old.get(asset['id'],{})
        if prior.get('english_sha256')==asset['english_sha256']:
            asset['visual_review']=prior.get('visual_review','pending')
        if asset['category']=='font':
            asset['english_status']='original_font_retained'
    rows=sorted(assets.values(),key=lambda r:(r['category'],r['name'],r['id']))
    registry=dict(schema='macross.localization.image-registry.v1',
        source_of_scope='Actual installed Korean DDS differences from original Japanese, primary and secondary routes',
        japanese_pack_root=str(roots[0]),korean_pack_root=str(roots[1]),english_pack_root=str(roots[2]),
        asset_count=len(rows),ui_asset_count=sum(r['category']=='ui_image' for r in rows),
        ui_route_count=len(ui),additional_pending=sum(r['scope']=='additional_original' and r['english_status']=='not_replaced' for r in rows),assets=rows,
        limitations=['Hash differences prove resource replacement, not English text accuracy or visible rendering.',
                     'Japanese content never changed in Korean is outside this baseline and must be added as discovered.',
                     'GOP/ELF strings, pilot names, dialogue and movie sidecars are separate text resources.'])
    write(registry_path,registry)
    write(registry_root/'registry.json',registry)
    with (registry_root/'registry.csv').open('w',encoding='utf-8-sig',newline='') as stream:
        writer=csv.writer(stream)
        writer.writerow(['ID','Image','Width','Height','Korean SHA256','English SHA256','English status','Visual review','Routes','Japanese file','Korean file','English file'])
        for r in rows:writer.writerow([r['id'],r['name'],r['width'],r['height'],r['korean_sha256'],r['english_sha256'],r['english_status'],r['visual_review'],len(r['routes']),*[r['source_files'][k] for k in ('ja-JP','ko-KR','en-US')]])
    md=['# 한국어·영어 이미지 적용 관리표','',
        f'한국어 적용 기준 참조 {len(ui)}곳. 추가 발견한 원본 이미지까지 UI {registry["ui_asset_count"]}종을 관리하며, 추가 영어 미적용 {registry["additional_pending"]}종이다. 글꼴 페이지는 별도 분류한다.',
        '', '영어 상태의 `적용·검수 대기`는 파일 적용만 확인한 상태다. 실제 화면에서 일본어 잔존·잘림·겹침을 확인하기 전에는 검수 완료로 올리지 않는다.',
        '', '[원본·한국어·영어 파일 및 상세 경로](../localization/image-registry/registry.json) · [CSV 관리표](../localization/image-registry/registry.csv)',
        '', '| ID | 이미지 | 한국어 적용 경로 수 | 영어 적용 | 화면 검수 |','|---|---|---:|---|---|']
    for r in rows:
        status='원본 글꼴 유지' if r['category']=='font' else '적용·검수 대기' if r['english_status'].startswith('installed') else '미적용'
        md.append(f'| {r["id"]} | {r["name"]} | {len(r["routes"])} | {status} | {r["visual_review"]} |')
    (ROOT/'docs/localization-image-register.md').write_text('\n'.join(md)+'\n',encoding='utf8')
    write(ROOT/'localization/en-US/images/korean-parity-audit.json',dict(
        preview=str(roots[2].parents[3]),
        korean_changed_image_routes=len(changed),ui_image_routes=len(ui),english_replaced_routes=len(ui)-len(missing),
        ui_asset_count=registry['ui_asset_count'],additional_pending=registry['additional_pending'],
        original_font_pages_retained=font,missing=missing,routes=changed,game_rendering_verified=False))
    print(json.dumps(dict(korean_changed_image_routes=len(changed),ui_image_routes=len(ui),english_replaced_routes=len(ui)-len(missing),
                          missing_routes=len(missing),missing_names=sorted({r['name'] for r in missing}))))


if __name__=='__main__':main()
