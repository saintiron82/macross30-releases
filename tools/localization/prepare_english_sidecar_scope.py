"""Freeze remaining English movie, typed ELF display and accepted ASR scopes."""
import json
from pathlib import Path
from english_pack import ROOT,digest,write

def read(path):return json.loads(path.read_text(encoding='utf8'))

if __name__=='__main__':
    jobs=[]
    for path in sorted((ROOT/'localization/en-US/subtitles/catalogs').glob('*.json')):
        doc=read(path);transcript=ROOT/doc['source_transcript']
        assert digest(transcript.read_bytes())==doc['source_sha256']
        for cue in doc['cues']:
            if cue['translation_en']:continue
            jobs.append(dict(id=f'movie:{path.stem}:{cue["id"]}',source=cue['source_ja'],kind='movie',catalog=str(path),cue_id=cue['id'],
                             transcript_sha256=doc['source_sha256'],movie_sha256=doc['movie_sha256'],start=cue['start'],end=cue['end'],review_flags=cue.get('review_flags',[])))
    path=ROOT/'localization/image_assets/data-ui-v6/catalog-translations-integrated.json'
    doc=read(path);elf=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes();assert digest(elf)==doc['source_sha256']
    for row in doc['entries']:
        if not row.get('translation_approved') or row.get('installation_excluded') or row.get('inplace_excluded'):continue
        offset=int(row['file_offset'],16);assert elf[offset:offset+len(row['source'].encode())+1]==row['source'].encode()+b'\0'
        jobs.append(dict(id='elf:'+row['file_offset'],source=row['source'],kind='elf',file_offset=row['file_offset'],
                         source_sha256=doc['source_sha256'],inventory_sha256=digest(path.read_bytes())))
    # Only stems present in the current accepted native catalog are in scope.
    native=ROOT/'native/macross30/battle_voice_catalog.inc'
    accepted=set()
    for line in native.read_text(encoding='utf8').splitlines():
        if line.lstrip().startswith('{'):
            pair=json.loads('['+line.strip().rstrip(',')[1:-1]+']');accepted.add(pair[0])
    candidates={}
    def add(stem,source,sha,source_path,provenance):
        if stem not in accepted:return
        record=dict(id='voice:'+stem,source=source,kind='voice',stem=stem,source_audio_sha256=sha,source_path=source_path,provenance=provenance)
        if stem in candidates and candidates[stem]['source_audio_sha256']!=sha:raise ValueError('Voice identity conflict')
        candidates[stem]=record
    folder=ROOT/'localization/subtitles/battle-voice'
    original=folder/'asr-caption-drafts.json'
    for row in read(original)['entries']:add(row['stem'],row['source_ja'],row['source_sha256'],row['source_path'],str(original))
    review=folder/'medium-crosscheck-v1'
    for path in [review/'candidate-review-v1/translation-reviewed.json',*sorted(review.glob('lexical-review-v*/translation-reviewed.json'))]:
        doc=read(path)
        for row in doc['entries']:
            if not row.get('publication_authorized'):continue
            source=row.get('reviewer_normalized_ja',row.get('Small_source_ja',''))
            if not source:raise ValueError('Missing accepted Japanese voice source')
            for asset in row['source_paths']:
                stem=asset.removeprefix('data/sound/voice/').removesuffix('.at3')
                add(stem,source,row['source_sha256'],asset,str(path))
    jobs.extend(candidates.values())
    assert len({r['id'] for r in jobs})==len(jobs)
    write(ROOT/'localization/en-US/model-drafts/sidecar-scope.json',dict(jobs=jobs,source_catalog_sha256=digest(native.read_bytes()),status='frozen_original_identity_scope'))
    print({kind:sum(r['kind']==kind for r in jobs) for kind in ('elf','movie','voice')})
