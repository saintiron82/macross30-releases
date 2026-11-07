"""Read-only caption evidence selection. Never updates translation or runtime files."""
import collections
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'localization/subtitles/battle-voice/medium-crosscheck-v1'
OUT = BASE / 'candidate-review-v1'

REASONS = {
    12: 'Both models preserve the complete reported order up to its deliberate interrupted ending; existing Korean preserves that ending without supplying a missing predicate.',
    15: 'Both models agree on meaningful complete sentences about investigators and cooperation; existing Korean conveys both clauses without an added subject or instruction.',
    31: 'The deictic phrase remains deictic in Korean; no target or cause is invented. Both models agree apart from punctuation.',
    38: 'Both models agree on a request for support and an interrupted identification; Korean preserves interruption without supplying the unit name.',
    67: 'Identical complete battle-start declaration; Korean directly conveys it.',
    76: 'Identical meaningful challenge rather than an isolated grunt; existing Korean directly conveys it.',
    83: 'Same complete prohibition in both models, punctuation only differs; Korean directly conveys it.',
    89: 'Identical meaningful surprised question about being defeated; Korean preserves its elliptical subject.',
    93: 'Identical conventional battle challenge; existing Korean is a complete natural equivalent without guessing a target.',
    94: 'Identical complete request for another shot; Korean directly conveys it.',
    96: 'Identical meaningful declaration of being able to continue; existing Korean directly conveys it.',
    99: 'Identical meaningful declaration of an easy victory; existing Korean directly conveys it.',
    104: 'Identical meaningful statement of having been saved; existing Korean is a natural complete equivalent.',
}


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quality(record):
    cues = record['segments']
    probs = [w['probability'] for s in cues for w in s.get('words', [])]
    return dict(min_avg_logprob=min((s['avg_logprob'] for s in cues), default=None),
                max_no_speech_prob=max((s['no_speech_prob'] for s in cues), default=None),
                mean_word_probability=sum(probs)/len(probs) if probs else None,
                raw_cue_span_seconds=sum(max(0, s['end']-s['start']) for s in cues),
                flags=sorted({f for s in cues for f in s['review_flags']}))


def main():
    progress = load(BASE/'progress.json')
    if progress['status'] != 'completed_unreviewed_Medium_crosscheck' or progress['failed']:
        raise ValueError('Medium must be terminal and complete')
    from transcribe_unresolved_battle_voice_batch import owner_is_live
    if owner_is_live(progress['pid']):
        raise ValueError('Previous ASR process still live')
    rows = load(BASE/'review-index.json')['entries']
    plan = {r['priority_index']: r for r in load(BASE/'plan.json')['entries']}
    contexts = []
    for table in ('battletext', 'scenariotext', 'systemtext', 'subquesttext'):
        p = ROOT/f'analysis/localization/voice-only-audit/gop_{table}-voice-map.json'
        contexts.extend(load(p)['records'])
    candidates, held = [], []
    blocked = {'low_average_log_probability', 'possible_non_speech',
               'possible_generic_ASR_hallucination', 'very_short_reaction_requires_listening'}
    for row in rows:
        entry = plan[row['index']]
        medium = load(BASE/row['checkpoint'])
        small = load(ROOT/entry['Small_transcript'])
        if sha(ROOT/entry['Small_transcript']) != entry['Small_transcript_sha256']:
            raise ValueError('Small source changed')
        qs, qm = quality(small), quality(medium)
        common = dict(index=row['index'], source_sha256=row['source_sha256'],
            source_paths=row['source_paths'], resolved_stems=[p.removeprefix('data/sound/voice/').removesuffix('.at3') for p in row['source_paths']],
            audio_sha256=entry['audio_sha256'], audio=entry['audio'], duration=medium['duration'],
            Small_source_ja=row['Small'], Medium_source_ja=row['Medium'], comparison=row['comparison'],
            lexical_normalized_match=row['lexical_normalized_match'], Small_quality=qs, Medium_quality=qm,
            checkpoint=row['checkpoint'], checkpoint_sha256=sha(BASE/row['checkpoint']),
            Small_translation_input_sha256=entry['batch_input']['Small_translation_input_sha256'],
            initial_KO_draft_sha256=entry['batch_input']['initial_KO_draft_sha256'],
            human_listening_verified=False)
        # Only context lookup: a same numeric voice key does NOT establish asset identity.
        keys = {Path(p).stem for p in row['source_paths']}
        contextual = [dict(table=c.get('table'),record_id=c['record_id'],source_ja=c['source'],
                           translation_ko=c.get('translation',''),resolved_stem=c.get('resolved_stem'),
                           identity='same voice-key context only; different full stem is not an exact audio match')
                      for c in contexts if c.get('voice_key') in keys and c.get('source')]
        common['original_source_context'] = contextual
        if row['index'] in REASONS:
            if not row['lexical_normalized_match'] or not row['initial_translation_ko'] or row['duration'] < 1.7:
                raise ValueError('Candidate selection invariant')
            if blocked.intersection(qm['flags'] + qs['flags']):
                raise ValueError('Candidate has a blocking ASR flag')
            ko = row['initial_translation_ko']
            if any(s in ko for s in ('청취', '원음', '확인 필요', '잠정', '미검수')):
                raise ValueError('Inline work annotation in candidate')
            if len(ko.encode('utf-8')) > 254:
                raise ValueError('Candidate requires timed cues')
            common.update(status='candidate_existing_KO_dual_ASR', translation_ko=ko,
                reason=REASONS[row['index']], runtime_reachability='not established by audio existence',
                publication_authorized=False)
            candidates.append(common)
        else:
            reasons=[]
            if not row['lexical_normalized_match']: reasons.append('models_disagree_beyond_punctuation')
            if not row['initial_translation_ko']: reasons.append('no_existing_complete_KO')
            if row['duration'] < 1.7: reasons.append('short_ambiguous_or_reaction_requires_context')
            reasons.extend(sorted(blocked.intersection(qm['flags'] + qs['flags'])))
            if not reasons: reasons.append('manual_semantic_hold_despite_model_agreement')
            common.update(status='held', reasons=reasons, initial_uncertainty_note=row['initial_uncertainty_note'],
                          next_action='Compare this exact stem with its original source context and audio; do not infer missing buttons, names, or nonverbal captions.')
            held.append(common)
    # Longer transcribed spans and useful model confidence are review priorities,
    # never proof that the clip contains speech or that recognition is correct.
    held.sort(key=lambda r:(bool(blocked.intersection(r['Medium_quality']['flags'])),
                             -r['Medium_quality']['raw_cue_span_seconds'],
                             -(r['Medium_quality']['mean_word_probability'] or 0)))
    for n,r in enumerate(held,1): r['review_priority']=n
    OUT.mkdir(exist_ok=True)
    for name, value in [('candidates.json', dict(status='review_candidates_only', entries=candidates)),
                        ('held-priorities.json', dict(status='unresolved_internal_QA', entries=held)),
                        ('validation.json',dict(status='PASS', total=182,candidates=len(candidates),held=len(held),
                            unique_source_sha256=len({r['source_sha256'] for r in candidates+held}),
                            source_review_index_sha256=sha(BASE/'review-index.json'),
                            source_plan_sha256=sha(BASE/'plan.json'), no_ASR_process_live=True,
                            source_or_runtime_modified=False,new_ASR_inferences=0,
                            human_listening_verified=False,publication_authorized=False,
                            candidate_max_KO_UTF8_bytes=max(len(r['translation_ko'].encode('utf-8')) for r in candidates),
                            held_reason_counts=dict(collections.Counter(x for r in held for x in r['reasons']))))]:
        (OUT/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(load(OUT/'validation.json'),ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
