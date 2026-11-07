"""Register baked Japanese loading names beyond the Korean image baseline."""
from english_pack import ROOT,write
from build_english_ui_images import read


if __name__=='__main__':
    diagnosis=read(ROOT/'analysis/localization/loading-name-audit/loading-name-source-diagnosis.json')
    copies=read(ROOT/'analysis/localization/loading-name-audit/current-character-loading-copies.json')
    names=read(ROOT/'localization/en-US/catalogs/pilot-names.json')
    by_filename={r['name']:r for r in copies['copies'] if r['role']=='disc' and r['route']=='primary'}
    rows=[]
    for character in diagnosis['characters']:
        for filename in character['full_and_small_filenames']:
            source=by_filename[filename]
            rows.append(dict(name=filename,source_ja=character['source_name'],translation_en=names[character['source_name']],
                             original_sha256=source['raw_sha256'],primary_record=source['record'],
                             english_status='not_started',visual_review='pending',
                             scope_reason='Baked Japanese loading-picture name, never replaced in Korean baseline',
                             edit_constraint='Detailed opaque artwork under text; reviewed local reconstruction required; preserve all outside the approved name mask.'))
    write(ROOT/'tools/localization/catalogs/additional_image_scope.json',dict(assets=rows))
    print(f'Registered {len(rows)} loading-picture image variants; not marked translated or installed.')
