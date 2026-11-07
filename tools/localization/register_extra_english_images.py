"""Register exact additional original image identities without claiming installation."""
import json
import argparse
from pathlib import Path
from english_pack import ROOT,digest,write
from stage_gop_primary_mirrors import pidx,read_payload

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--manifest',type=Path,default=ROOT/'localization/en-US/images/extra-ui-v1/manifest.json');args=parser.parse_args()
    manifest=json.loads(args.manifest.read_text(encoding='utf8'))
    path=ROOT/'tools/localization/catalogs/additional_image_scope.json';scope=json.loads(path.read_text(encoding='utf8'))
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK';_,rows,_=pidx(pack/'PACK.IDX');paths={p.name.lower():p for p in pack.iterdir()}
    old={(r['name'],r['original_sha256'],r['primary_record']) for r in scope['assets']};added=0
    candidates={(r['name'],r['source_sha256']):r for r in manifest['assets']}
    for row in rows:
        if row['name'] not in {k[0] for k in candidates}:continue
        source,_=read_payload(paths[Path(row['pack']).name.lower()],row);sha=digest(source)
        item=candidates.get((row['name'],sha))
        if not item:raise ValueError('Unknown additional original variant')
        key=(row['name'],sha,row['record'])
        if key in old:continue
        scope['assets'].append(dict(name=row['name'],source_ja='Pinned original Japanese text sprite; see source DDS and contact sheet',translation_en=' | '.join(r['text'] for r in item['labels']),original_sha256=sha,primary_record=row['record'],english_status='prepared_not_installed',visual_review='pending',scope_reason='Japanese text found outside the Korean replacement baseline',credit_name_readings_review=item.get('name_readings_review')))
        added+=1
    write(path,scope);print(dict(added_primary_routes=added,additional_scope_routes=len(scope['assets'])))
