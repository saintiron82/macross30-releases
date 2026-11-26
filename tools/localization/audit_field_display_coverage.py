"""Read-only Japanese field-name coverage and Korean GOP route audit.

Translation matches are evidence for reuse, not automatic authoring decisions.
CSV records may describe allies, story actors, devices, or unused definitions;
this audit does not claim that every record is an enemy or visible on screen.
"""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import re
import struct
import unicodedata

from stage_gop_primary_mirrors import pidx, read_payload, fsts_entries
from stage_hud_guide import sha, sha_file
from stage_nested_ui import region
from gop_strings import parse

ROOT = Path(__file__).resolve().parents[2]
JP = re.compile(r'[\u3041-\u3096\u30a1-\u30fa\u3400-\u9fff]')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def normalized(text):
    return re.sub(r'\s+', '', unicodedata.normalize('NFKC', text))


def audit(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if not output.is_relative_to(ROOT / 'build/tests'):
        raise ValueError('Reports must stay in build/tests')
    output.mkdir(parents=True, exist_ok=True)
    _, primary, archives = pidx(source / 'pack.idx')
    catalogs = {p.stem: read(p) for p in sorted((ROOT / 'localization/ko-KR/structured').glob('*.json'))}
    terms, all_prose = defaultdict(list), []

    def add(original, translated, evidence):
        if original and translated and JP.search(original) and not JP.search(translated):
            row = dict(translation=translated, evidence=evidence)
            if row not in terms[original]:
                terms[original].append(row)

    for name, catalog in catalogs.items():
        for row in catalog['entries']:
            evidence = f'localization/ko-KR/structured/{name}.json#{row["index"]}'
            add(row['source'], row.get('translation', ''), evidence)
            all_prose.append((row['source'], row.get('translation', ''), evidence))
    for row in read(ROOT / 'localization/ko-KR/ui_fixed_names.json')['patch_candidates']:
        add(row['source'], row['translation'], 'ui_fixed_names.json#' + row['file_offset'])
    active = read(ROOT / 'localization/image_assets/active-ui.json')
    elf_path = ROOT / 'localization/image_assets' / active['bundle'] / active['elf_override']['path']
    elf = elf_path.read_bytes()
    if sha(elf) != active['elf_override']['sha256']:
        raise ValueError('Active Korean ELF changed')
    pooled_remaining, pointer_reads = [], 0
    for row in read(ROOT / 'localization/en-US/build/full-elf-manifest.json')['assignments']:
        for ref in row['typed_pointer_file_offsets']:
            target = struct.unpack_from('>I', elf, int(ref, 0))[0] - 0x10000
            if not 0 <= target < len(elf):
                raise ValueError('Display pointer outside ELF')
            value = elf[target:elf.index(0, target)].decode('utf8')
            add(row['source'], value, 'active_elf#' + ref)
            pointer_reads += 1
            if JP.search(value):
                pooled_remaining.append(dict(source=row['source'], current=value, pointer=ref))
    normalized_terms = defaultdict(list)
    for original, matches in terms.items():
        normalized_terms[normalized(original)].extend(matches)
    prepared = {e['source']: e['translation'] for e in read(ROOT / 'localization/ko-KR/field_enemy_names.json')['entries']}
    ready_path = ROOT / 'localization/ko-KR/field-display-names-ready.json'
    if ready_path.is_file():
        ready = read(ready_path)
        current_catalog = ROOT / ready['catalog']
        artifact = ROOT / ready['package']
        if (sha_file(current_catalog) != ready['catalog_sha256'] or not artifact.is_file()
                or sha_file(artifact) != ready['sha256']):
            raise ValueError('Prepared field-name candidate is missing or changed')
        prepared = {r['source']: r['translation'] for r in read(current_catalog)['labels']}
    names, files, errors = defaultdict(list), {}, []
    other_csv = []
    for row in primary:
        name = row['name']
        if not name.lower().endswith('.csv'):
            continue
        raw = read_payload(source / Path(row['pack']).name, row)[0]
        if name in files:
            if files[name]['sha256'] != sha(raw):
                errors.append(dict(resource=name, error='different primary CSV variants'))
            files[name]['primary_aliases'] += 1
            continue
        try:
            text, encoding = raw.decode('utf8'), 'utf-8'
        except UnicodeDecodeError:
            text, encoding = raw.decode('cp932'), 'cp932'
        lines = text.splitlines()
        header = next((next(csv.reader([line]), []) for line in lines if line.startswith('#,NAME,')), [])
        info = dict(sha256=sha(raw), encoding=encoding, primary_aliases=1,
                    placement_schema=header[:5] == ['#', 'NAME', '表示名', '搭乗者', 'ID'],
                    secondary_routes=[], secondary_variants=[])
        files[name] = info
        if not info['placement_schema']:
            japanese_lines = [line for line in lines if JP.search(line)]
            other_csv.append(dict(resource=name, first_lines=lines[:2], japanese_present=bool(japanese_lines),
                                  japanese_line_count=len(japanese_lines), japanese_samples=japanese_lines[:4],
                                  provisional_role='AI function-name table; preserve identifiers' if name.startswith('tri')
                                  else 'non-placement data; not classified as display text'))
            continue
        for line_number, line in enumerate(lines, 1):
            values = next(csv.reader([line]), [])
            if len(values) < 5 or values[0].startswith('#') or not JP.search(values[2]):
                continue
            unit = values[4]
            kind = 'unit' if unit.startswith(('MB_', 'MF_', 'MG_', 'MR_')) else 'world_object_or_ship'
            names[values[2]].append(dict(resource=name, line=line_number, object_id=values[1],
                                         pilot_id=values[3], machine_id=unit, kind=kind,
                                         allegiance=values[11] if len(values) > 11 else ''))
    # Read secondary CSV variants too; basename only links the report. All exact
    # full resource paths and payload hashes are retained for future patching.
    for row in archives:
        pack = source / Path(row['pack']).name
        if region(pack, row['offset'], 4) != b'FSTS':
            continue
        count, table, start, size = struct.unpack_from('<4I', region(pack, row['offset'], 20), 4)
        if start + size > row['size']:
            raise ValueError('Archive name table outside bounds')
        table_names = region(pack, row['offset'] + start, size)
        paths = [s.decode('utf8') for s in table_names.split(b'\0') if s.lower().endswith(b'.csv')]
        if not any(Path(path).name in files for path in paths):
            continue
        blob = read_payload(pack, row)[0]
        for entry in fsts_entries(blob):
            basename = Path(entry['name']).name
            if basename not in files:
                continue
            digest = sha(entry['payload'])
            route = dict(archive=row['name'], pack=Path(row['pack']).name, path=entry['name'], sha256=digest)
            files[basename]['secondary_routes'].append(route)
            if digest != files[basename]['sha256']:
                files[basename]['secondary_variants'].append(route)
    results = []
    for original, occurrences in sorted(names.items(), key=lambda x: (-len(x[1]), x[0])):
        exact = terms.get(original, [])
        equivalent = normalized_terms.get(normalized(original), []) if not exact else []
        candidates = exact or equivalent
        unique = sorted({r['translation'] for r in candidates})
        contexts = [dict(source=s, translation=t, evidence=e) for s, t, e in all_prose if original in s][:3]
        status = ('prepared_patch' if original in prepared else
                  'existing_translation_conflict' if len(unique) > 1 else
                  'existing_exact_translation' if exact else
                  'existing_normalized_translation' if equivalent else 'no_full_label_translation')
        results.append(dict(source=original, status=status, existing_translations=unique,
                            translation_evidence=candidates, context_matches=contexts,
                            prepared_translation=prepared.get(original), occurrences=len(occurrences),
                            kinds=sorted({r['kind'] for r in occurrences}), definitions=occurrences))
    gop_readbacks = []
    gop_archives = [row for row in archives if row['name'] == 'gop']
    secondary_gops = {}
    for row in gop_archives:
        for entry in fsts_entries(read_payload(source / Path(row['pack']).name, row)[0]):
            secondary_gops[entry['name']] = entry['payload']
    for name, catalog in catalogs.items():
        routes = [(f'primary:{r["record"]}', read_payload(source / Path(r['pack']).name, r)[0])
                  for r in primary if r['name'] == name + '.gop']
        key = 'data/gop/' + name + '.gop'
        if key in secondary_gops:
            routes.append(('secondary:' + key, secondary_gops[key]))
        for route, raw in routes:
            actual = parse(raw)
            mismatches, japanese = [], []
            for row in catalog['entries']:
                value = actual[row['index']]['text']
                if value != row.get('translation', ''):
                    mismatches.append(dict(index=row['index'], source=row['source'], current=value,
                                           expected=row.get('translation', '')))
                if JP.search(value):
                    japanese.append(dict(index=row['index'], current=value))
            gop_readbacks.append(dict(resource=name, route=route, checked=len(catalog['entries']),
                                      mismatch_count=len(mismatches), japanese_count=len(japanese),
                                      mismatches=mismatches, japanese=japanese))
    counts = Counter(r['status'] for r in results)
    summary = dict(csv_files=len(files), placement_csv_files=sum(r['placement_schema'] for r in files.values()),
                   japanese_label_types=len(results), japanese_definitions=sum(r['occurrences'] for r in results),
                   prepared_types=counts['prepared_patch'], remaining_types=len(results)-counts['prepared_patch'],
                   remaining_definitions=sum(r['occurrences'] for r in results if r['status'] != 'prepared_patch'),
                   status_counts=dict(counts),
                   csv_secondary_routes=sum(len(r['secondary_routes']) for r in files.values()),
                   csv_secondary_variant_routes=sum(len(r['secondary_variants']) for r in files.values()),
                   gop_routes=len(gop_readbacks), gop_values_checked=sum(r['checked'] for r in gop_readbacks),
                   gop_mismatches=sum(r['mismatch_count'] for r in gop_readbacks),
                   gop_japanese_values=sum(r['japanese_count'] for r in gop_readbacks),
                   elf_pointer_reads=pointer_reads, elf_reviewed_pointer_japanese=len(pooled_remaining), errors=errors)
    report = dict(summary=summary, source_pack_directory=source.relative_to(ROOT).as_posix(),
                  source_index_sha256=sha_file(source / 'pack.idx'), active_elf_sha256=sha(elf),
                  field_labels=results, csv_resources=files, other_csv_schemas=other_csv,
                  gop_readbacks=gop_readbacks, elf_remaining=pooled_remaining,
                  limitations=['Read-only audit; no game, installation, translation or package changed.',
                               'Field counts are definitions, including allies/devices/unused objects, not observed enemies.',
                               'Existing translation matches are suggestions requiring contextual review.',
                               'Prepared two-name patch is accounted for separately; source packs remain unchanged.',
                               'Image text, video text, raw ELF debug/music literals, and non-placement CSV schemas are not certified by this audit.'])
    (output / 'AUDIT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    with (output / 'FIELD-LABELS.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['source_ja', 'status', 'existing_ko', 'definitions', 'kinds', 'example_resource', 'example_object'])
        for row in results:
            example = row['definitions'][0]
            writer.writerow([row['source'], row['status'], ' / '.join(row['existing_translations']), row['occurrences'],
                             ' / '.join(row['kinds']), example['resource'], example['object_id']])
    with (output / 'REMAINING-FIELD-LABELS.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['日本語表示名', '既存の韓国語対応値', '対応状況', '定義数', 'ファイル例', 'オブジェクトID例'])
        for row in results:
            if row['status'] == 'prepared_patch':
                continue
            example = row['definitions'][0]
            writer.writerow([row['source'], ' / '.join(row['existing_translations']), row['status'],
                             row['occurrences'], example['resource'], example['object_id']])
    exact = [r for r in results if r['status'] == 'existing_exact_translation']
    pending = [r for r in results if r['status'] == 'no_full_label_translation']
    lines = ['필드/미션 표시 이름 미대응 조사 (2026-11-26)', '',
             '범위: build/game-ko-full의 배치 CSV 전체와 기존 한국어 번역 자료.',
             f'CSV {len(files)}개 중 배치 스키마 {summary["placement_csv_files"]}개.',
             f'일본어 표시 이름 {len(results)}종 / {summary["japanese_definitions"]}개 정의.',
             f'이 중 수정팩으로 준비한 {counts["prepared_patch"]}종·{sum(r["occurrences"] for r in results if r["status"] == "prepared_patch")}개 정의는 별도 처리.',
             f'나머지 {summary["remaining_types"]}종 / {summary["remaining_definitions"]}개 정의:',
             f'- 기존 전체 이름 대응값 있음: {len(exact)}종 / {sum(r["occurrences"] for r in exact)}개 정의.',
             f'- 전체 이름 대응값 없음: {len(pending)}종 / {sum(r["occurrences"] for r in pending)}개 정의.',
             '  이 항목들도 일부 용어와 문맥은 기존 번역에서 재사용할 수 있다.', '',
             '동일 누락 경로의 예:',
             '- バンデット → 밴디트: 322개 정의',
             '- ハーヴァマール → 하바말: 251개 정의',
             '- ゼントラーディ → 젠트라디: 193개 정의',
             '- チーム・フラミンゴ: 71개 정의 (기존 대사 문맥: 팀 플라밍고)',
             '- ラグナロク党: 65개 정의 (기존 문맥: 라그나로크당)',
             '- マニア倶楽部: 60개 정의 (새 이름 검토 필요)', '',
             '우로보로스 군도 mm_island.csv에 남은 5종:',
             'ラグナロク党 / マニア倶楽部 / ハンター / シャーク団 / カーゴ', '',
             '다른 지역의 문지기 이름:',
             'ドウェルグの門番 / オルコスの門番 / ノモスの門番 / ファヴニールの門番',
             '기존 장소 대응값: 드베르그 동굴 / 오르코스 기지 / 노모스 기지 / 파프니르 동굴.', '',
             '추가 확인:',
             f'- CSV 아카이브 경로 {summary["csv_secondary_routes"]}개: 직접 파일과 다른 변형 0개.',
             '- 번역 GOP 12종의 직접/아카이브 24개 경로, 27,072개 문자열 대조: 불일치 0개.',
             '- ELF의 검토된 표시 포인터 1,051개: 일본어 잔존 0개.',
             '- 별도 tri*.csv 7개는 일본어 AI 함수명 표로 보이며 번역 대상이라고 판단하지 않는다.', '',
             '주의: 정의 수는 실제 등장 적 수가 아니다. 아군, 인물, 장치, 미사용 테스트도 포함.',
             '화면 노출 여부와 사용 조건은 별도 확인이 필요하다.',
             '이미지/동영상 텍스트 및 ELF의 미분류 음악/내부 문자열까지 완전 검수한 결과는 아니다.',
             '이번 조사는 읽기 전용이며 번역, 설치본, 세이브, 배포팩을 수정하지 않았다.', '',
             '전체 근거: AUDIT.json', '나머지 목록: REMAINING-FIELD-LABELS.csv', '준비된 수정 포함 전체: FIELD-LABELS.csv']
    (output / 'REPORT.ko.txt').write_text('\n'.join(lines) + '\n', encoding='utf8')
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT / 'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK')
    ap.add_argument('--output', type=Path, default=ROOT / 'build/tests/field-display-coverage-20261126')
    args = ap.parse_args()
    audit(args.source, args.output)
