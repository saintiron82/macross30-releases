"""Prepare English movie catalogs with pinned Japanese transcripts and original PTS."""
import json
import math
import textwrap
import argparse
from pathlib import Path
from english_pack import ROOT, digest, write


def stamp(seconds):
    ms = round(seconds * 1000)
    hours, ms = divmod(ms, 3600000)
    minutes, ms = divmod(ms, 60000)
    seconds, ms = divmod(ms, 1000)
    return f'{hours:02}:{minutes:02}:{seconds:02},{ms:03}'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-long-cues',action='store_true',help='Preserve all words and original outer timing while paging long cues')
    args=parser.parse_args()
    source_root = ROOT / 'localization/subtitles'
    base = ROOT / 'localization/en-US/subtitles'
    line_seeds = json.loads((ROOT / 'localization/en-US/catalogs/movie-lines-v1.json').read_text(encoding='utf-8'))['translations']
    clips = {}
    for path in sorted((source_root / 'translation-batches').glob('batch-*.ko.json')):
        for clip in json.loads(path.read_text(encoding='utf-8'))['clips']:
            movie = clip['movie'].upper()
            if movie in clips:
                raise ValueError(f'Duplicate movie: {movie}')
            clips[movie] = clip
    tracks = (source_root / 'runtime/tracks.tsv').read_text(encoding='utf-8').splitlines()
    if tracks[0] != 'MACROSS_SUBTITLES_V1':
        raise ValueError('Unknown track registry')
    report, registry = [], ['MACROSS_SUBTITLES_V1']
    projections=[]
    for line in tracks[1:]:
        if not line.strip():
            continue
        movie, _, origin, _, pam_sha, _, _ = line.split('\t')
        key = Path(movie).stem.upper()
        if key == '020':
            transcript = source_root / 'asr/020-medium/transcript.ja.json'
            raw = json.loads(transcript.read_text(encoding='utf-8'))['segments']
        else:
            clip = clips[key]
            transcript = ROOT / clip['source_transcript']
            if digest(transcript.read_bytes()) != clip['source_sha256']:
                raise ValueError(f'Transcript hash mismatch: {key}')
            raw = json.loads(transcript.read_text(encoding='utf-8'))['segments']
            if [(r['id'], r['start'], r['end'], r['source_ja']) for r in raw] != [
                (r['id'], r['start'], r['end'], r['source_ja']) for r in clip['cues']]:
                raise ValueError(f'Transcript/timing mismatch: {key}')
        path = base / 'catalogs' / f'{key}.json'
        previous = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {'cues': []}
        old = {r['id']: r for r in previous['cues']}
        seed_file = ROOT / 'localization/en-US/catalogs' / f'movie-{key}-v1.json'
        seed = json.loads(seed_file.read_text(encoding='utf-8')) if seed_file.exists() else {}
        if seed and (seed['source_sha256'] != digest(transcript.read_bytes()) or len(seed['translations']) != len(raw)):
            raise ValueError(f'Stale movie translation seed: {key}')
        cues, end = [], 0
        for row in raw:
            if not math.isfinite(row['start'] + row['end']) or not end <= row['start'] < row['end']:
                raise ValueError(f'Invalid cue timing: {key}/{row["id"]}')
            prior = old.get(row['id'], {})
            identity = {k: row[k] for k in ('id', 'start', 'end', 'source_ja')}
            if prior and any(prior[k] != value for k, value in identity.items()):
                raise ValueError(f'Stale English cue: {key}/{row["id"]}')
            text = prior.get('translation_en') or (seed['translations'][len(cues)] if seed else line_seeds.get(row['source_ja'], ''))
            if '\x00' in text or '\n\n' in text:
                raise ValueError('Invalid SRT cue text')
            cues.append(dict(**identity, translation_en=text,
                             review_flags=row.get('review_flags', []), needs_audio_review=True))
            cues[-1].update({k:prior[k] for k in ('translation_status','translation_model') if k in prior})
            end = row['end']
        write(path, dict(locale='en-US', movie=movie, origin90k=int(origin), movie_sha256=pam_sha,
                         source_transcript=transcript.relative_to(ROOT).as_posix(),
                         source_sha256=digest(transcript.read_bytes()), cues=cues))
        translated = sum(bool(r['translation_en'].strip()) for r in cues)
        complete = translated == len(cues)
        if complete:
            rendered=[]
            from english_subtitle_layout import pages,project
            cue_pages=[pages(r['translation_en']) for r in cues]
            paged=any(len(parts)>1 for parts in cue_pages)
            runtime_id=0
            for r,parts in zip(cues,cue_pages):
                if args.project_long_cues:
                    for n,(start,end,text) in enumerate(project(r['start'],r['end'],parts)):
                        runtime_id+=1;identity=runtime_id if paged else r['id']
                        rendered.append(f'{identity}\n{stamp(start)} --> {stamp(end)}\n{text}')
                        projections.append(dict(movie=movie,source_cue_id=r['id'],runtime_cue_id=identity,page=n+1,pages=len(parts),source_start=r['start'],source_end=r['end'],start=start,end=end))
                    continue
                lines=textwrap.wrap(' '.join(r['translation_en'].split()),38,
                                    break_long_words=False,break_on_hyphens=False)
                if len(lines)>3 or any(len(line)>38 for line in lines):
                    raise ValueError(f'English subtitle exceeds preview line limits: {key}/{r["id"]}')
                text='\n'.join(lines)
                rendered.append(f'{r["id"]}\n{stamp(r["start"])} --> {stamp(r["end"])}\n{text}')
            srt = '\n\n'.join(rendered) + '\n'
            output = base / 'review' / f'{key}.en.srt'
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(srt.encode('utf-8'))
            # Disabled until the complete pack and rendering have been reviewed.
            registry.append('\t'.join([movie, output.name, origin, '0', pam_sha, digest(output.read_bytes()), 'draft']))
        report.append(dict(movie=movie, cues=len(cues), translated=translated, complete=complete))
    review = base / 'review'
    review.mkdir(parents=True, exist_ok=True)
    (review / 'tracks.tsv').write_bytes(('\n'.join(registry) + '\n').encode('utf-8'))
    result = dict(locale='en-US', status='INCOMPLETE_NOT_INSTALLED', movies=report,
                  cues=sum(r['cues'] for r in report), translated=sum(r['translated'] for r in report),
                  timing_preserved=True, human_audio_verified=False, game_render_verified=False)
    if result['translated']==result['cues']:result['status']='FULL_DRAFT_REVIEW_PENDING'
    write(base / 'manifest.json', result)
    if args.project_long_cues:write(base/'cue-projection.json',dict(pages=projections,source_catalog_timing_unchanged=True,original_outer_intervals_preserved=True,all_English_words_preserved=True,subpage_timing='proportional text length; listening review pending'))
    print(json.dumps({k: v for k, v in result.items() if k != 'movies'}))


if __name__ == '__main__':
    main()
