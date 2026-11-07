"""Admit English movie drafts only after rechecking frozen cue identity and timing."""
import json
from english_pack import ROOT,digest,write
from translate_english_local import mask,validate
from english_sidecar_results import load_results

if __name__=='__main__':
    base=ROOT/'localization/en-US';scope_path=base/'model-drafts/sidecar-scope.json'
    scope,result=load_results()
    assert result['scope_sha256']==digest(scope_path.read_bytes())
    documents={};changed=[];missing=[]
    for job in scope['jobs']:
        if job['kind']!='movie':continue
        row=result['entries'].get(job['id'])
        if not row:missing.append(job['id']);continue
        path=__import__('pathlib').Path(job['catalog']);assert path.resolve().is_relative_to((base/'subtitles/catalogs').resolve())
        if path not in documents:documents[path]=json.loads(path.read_text(encoding='utf8'))
        doc=documents[path]
        assert doc['source_sha256']==job['transcript_sha256'] and doc['movie_sha256']==job['movie_sha256']
        assert digest((ROOT/doc['source_transcript']).read_bytes())==job['transcript_sha256']
        cue=next(r for r in doc['cues'] if r['id']==job['cue_id'])
        assert row['source']==cue['source_ja']==job['source'] and (cue['start'],cue['end'])==(job['start'],job['end'])
        if cue['translation_en']:continue
        masked,saved=mask(row['translation']);text,reason=validate(job['source'],masked,saved)
        if reason:raise ValueError(job['id']+': '+reason)
        cue.update(translation_en=text,translation_status='local_model_draft_needs_review',translation_model=row['model'],needs_audio_review=True)
        changed.append(job['id'])
    for path,doc in documents.items():write(path,doc)
    write(base/'subtitles/movie-draft-import.json',dict(imported=len(changed),missing=missing,source_timing_unchanged=True,movie_hashes_unchanged=True,source_transcripts_unchanged=True,human_translation_review=False))
    print(dict(imported=len(changed),missing=len(missing)))
