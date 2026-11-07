"""Read-only QA of all unresolved voice translation drafts and installed ABF.

Eligibility means mechanically usable *unverified draft*, never audio approval.
No translated text, catalog, source pack, or running process is changed.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import unicodedata

from audit_runtime_font import read_font
from stage_nested_ui import pidx, region, fsts_entries, decoded

ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/subtitles/battle-voice'
FONT_PATH='data/font/font_j24x24.abf'
FONT_SHA='1d5ba9ee7c35a254a4a8a5d50e89e57d4c3025eeceaa3bd7a1d97ccb8628046c'
SEVERE_FLAGS={
    'possible_generic_ASR_hallucination', 'possible_repetition',
    'possible_non_speech', 'low_average_log_probability',
    'very_short_reaction_requires_listening',
}
NEW_FIELDS={'translation_ko','status','uncertainty_note','requires_timed_cues'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def relative(path):
    return str(Path(path).resolve().relative_to(ROOT))


def installed_font():
    evidence=[];coverage=None
    for role,folder in (
        ('disc',ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'),
        ('hdd',ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'),
    ):
        master,archives=pidx(folder/'pack.idx')
        resident=[a for a in archives if a['path']=='resident']
        if len(resident)!=1:
            raise ValueError('Missing/ambiguous installed resident font archive')
        archive=resident[0]
        blob=region(folder/archive['pack'],archive['offset'],archive['size'])
        selected=[e for e in fsts_entries(blob) if e['path']==FONT_PATH]
        if len(selected)!=1:
            raise ValueError('Missing/ambiguous installed font ABF')
        entry=selected[0];raw,_=decoded(blob,entry)
        common,glyphs,pages=read_font(raw)
        actual={g['codepoint'] for g in glyphs}
        if sha(raw)!=FONT_SHA or len(glyphs)!=8175 or len(actual)!=8175:
            raise ValueError('Installed font is not the reviewed 8175-glyph epoch')
        if coverage is not None and coverage!=actual:
            raise ValueError('Disc/HDD font repertoire differs')
        coverage=actual
        # Re-read only bounded metadata/payload: no multi-GB pack hash is needed.
        if sha(region(folder/'pack.idx',0,len(master)))!=sha(master):
            raise ValueError('Source index changed during font read')
        evidence.append(dict(role=role,index=relative(folder/'pack.idx'),index_metadata_sha256=sha(master),
                             archive=archive,entry=entry,raw_sha256=sha(raw),glyph_count=len(glyphs),
                             pages=pages,atlas_dimensions=[common['width'],common['height']]))
    return coverage,dict(status='PASS',font_path=FONT_PATH,sha256=FONT_SHA,glyph_count=8175,
                         copies=evidence,disc_hdd_repertoire_identical=True)


def audit():
    coverage,font=installed_font()
    snapshots=[];batches=[];entries=[];seen_keys=set();missing_by_cp=defaultdict(list)
    flag_counts=Counter();severe_counts=Counter();statuses=Counter();durations=defaultdict(float)
    warnings=[];draft_severe=[];overlength=[];all_text=[]
    for batch in (1,2,3):
        input_path=BASE/f'translation-batches/batch-{batch}.json'
        output_path=BASE/f'translation-batches/batch-{batch}.ko.json'
        inp=input_path.read_bytes();out=output_path.read_bytes()
        source=json.loads(inp);translated=json.loads(out)
        if source['batch']!=batch or translated['batch']!=batch or translated['input_sha256']!=sha(inp):
            raise ValueError(f'Batch {batch}: source batch/hash identity changed')
        original=source['entries'];proposed=translated['entries']
        if len(original)!=len(proposed) or len(original)!=source['clips']:
            raise ValueError(f'Batch {batch}: omitted/extra entries')
        if {(e['source_sha256'],e['source_ja']) for e in original}!={(e['source_sha256'],e['source_ja']) for e in proposed}:
            raise ValueError(f'Batch {batch}: source identity set differs')
        batch_status=Counter();batch_seconds=0.0;batch_held=0
        for index,(old,new) in enumerate(zip(original,proposed)):
            if any(new.get(k)!=v for k,v in old.items()) or set(new)-set(old)-NEW_FIELDS:
                raise ValueError(f'Batch {batch} index {index}: source metadata/order changed')
            if new.get('status') not in ('draft','needs_audio_review','empty_asr'):
                raise ValueError('Unreviewed or invalid translation status')
            text=new.get('translation_ko');note=new.get('uncertainty_note')
            if not isinstance(text,str) or not isinstance(note,str):
                raise ValueError('Translation/note must be strings')
            duration=old['duration']
            if not isinstance(duration,(int,float)) or not math.isfinite(duration) or duration<0:
                raise ValueError('Invalid source duration')
            key=(batch,index);seen_keys.add(key)
            flags=set(old.get('review_flags',[]))
            for segment in old.get('segments',[]):flags.update(segment.get('review_flags',[]))
            severe=sorted(flags&SEVERE_FLAGS);flag_counts.update(flags);severe_counts.update(severe)
            count=len(text.encode('utf8'));missing=sorted({ord(c) for c in text if c not in '\n\r\t' and ord(c) not in coverage})
            reasons=[]
            if new['status']!='draft':reasons.append('translation_status_'+new['status'])
            if severe:reasons.append('severe_ASR_review_flags')
            if not text.strip():reasons.append('no_Korean_caption')
            if not old['source_ja'].strip():reasons.append('empty_ASR_source')
            if count>254:reasons.append('native_caption_exceeds_254_UTF8_bytes')
            if new.get('requires_timed_cues'):reasons.append('requires_timed_cues')
            if missing:reasons.append('missing_installed_font_glyphs')
            if '\0' in text:reasons.append('embedded_NUL')
            eligibility='held' if reasons else 'eligible_draft'
            reference=dict(batch=batch,entry_index=index,source_sha256=old['source_sha256'],source_paths=old['source_paths'])
            row=dict(**reference,source_ja=old['source_ja'],translation_ko=text,status=new['status'],
                     native_speaker_IDSTR_present=old['native_speaker_IDSTR_present'],
                     uncertainty_note=note,duration_seconds=duration,review_flags=sorted(flags),severe_ASR_flags=severe,
                     UTF8_bytes=count,requires_timed_cues=bool(new.get('requires_timed_cues') or count>254),
                     missing_codepoints=[f'U+{c:04X}' for c in missing],eligibility=eligibility,hold_reasons=reasons)
            entries.append(row);all_text.append(text)
            statuses[new['status']]+=1;batch_status[new['status']]+=1
            durations[new['status']]+=duration;durations[eligibility]+=duration
            batch_seconds+=duration;batch_held+=eligibility=='held'
            if new['status']=='draft' and severe:draft_severe.append(reference|dict(severe_ASR_flags=severe))
            if count>254:overlength.append(reference|dict(UTF8_bytes=count,requires_timed_cues_declared=bool(new.get('requires_timed_cues'))))
            for cp in missing:missing_by_cp[cp].append(reference|dict(status=new['status'],translation_ko=text))
        batches.append(dict(batch=batch,input=relative(input_path),input_sha256=sha(inp),output=relative(output_path),
                            output_sha256=sha(out),entries=len(original),source_identities_exact=True,
                            all_original_metadata_and_order_preserved=True,status_counts=dict(batch_status),
                            duration_seconds=round(batch_seconds,6),eligible_draft=len(original)-batch_held,held=batch_held))
        snapshots.extend(((input_path,sha(inp)),(output_path,sha(out))))
    if len(entries)!=413 or len(seen_keys)!=413:
        raise ValueError('Expected every one of 413 translation entries')
    for path,digest in snapshots:
        if sha(path.read_bytes())!=digest:raise ValueError('Translation input/output changed during audit')
    missing_rows=[dict(codepoint=f'U+{cp:04X}',character=chr(cp),unicode_name=unicodedata.name(chr(cp),'UNNAMED'),
                       affected_entries=len(uses),attribution=uses) for cp,uses in sorted(missing_by_cp.items())]
    eligible=sum(e['eligibility']=='eligible_draft' for e in entries)
    return dict(status='PASS',scope='Read-only mechanical QA; eligible_draft is not listened/verified audio',
                human_listening_verified=False,game_launched=False,active_modified=False,translated_text_modified=False,
                batches=batches,total_entries=len(entries),source_audio_hashes_unique=len({e['source_sha256'] for e in entries}),
                source_path_aliases=sum(len(e['source_paths']) for e in entries),status_counts=dict(statuses),
                duration_seconds=sum(e['duration_seconds'] for e in entries),
                duration_by_status_or_eligibility_seconds={k:round(v,6) for k,v in durations.items()},
                review_flag_counts=dict(flag_counts),severe_ASR_flag_counts=dict(severe_counts),
                severe_ASR_flags_policy=sorted(SEVERE_FLAGS),drafts_with_severe_ASR_flags=draft_severe,
                native_caption_limit_UTF8_bytes=254,overlength_entries=overlength,
                maximum_proposed_caption_UTF8_bytes=max(e['UTF8_bytes'] for e in entries),
                installed_font=font,proposed_Korean_unique_rendered_codepoints=len({ord(c) for t in all_text for c in t if c not in '\n\r\t'}),
                missing_codepoints=missing_rows,missing_codepoint_count=len(missing_rows),
                eligible_draft=eligible,held=len(entries)-eligible,
                eligible_draft_entry_keys=[dict(batch=e['batch'],entry_index=e['entry_index'],source_sha256=e['source_sha256']) for e in entries if e['eligibility']=='eligible_draft'],
                entries=entries,
                limitations=['No original audio was listened to by this audit.',
                             'No gameplay voice timing, UI layout, subtitle display, or audio-to-text correctness is verified.',
                             'Unflagged draft text can still be wrong; severe-flag policy is an additional mechanical hold.',
                             'Eligibility does not prove speaker alias mapping or runtime voice admission.',
                             'Font coverage checks installed ABF codepoint presence, not visual glyph quality.',
                             'Duration totals count each source entry once, not each alias path.',
                             'Newline/CR/tab are layout controls and are excluded from font-glyph lookup.'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=BASE/'translation-review-v1.json')
    args=parser.parse_args();out=args.output.resolve()
    if BASE.resolve() not in out.parents:
        raise ValueError('Audit output must stay within battle-voice artifacts')
    result=audit();out.parent.mkdir(parents=True,exist_ok=True)
    out.write_bytes((json.dumps(result,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
    print(json.dumps({k:result[k] for k in ('status','total_entries','status_counts','eligible_draft','held','missing_codepoint_count')},ensure_ascii=False,indent=2))
    print('draft_severe='+str(len(result['drafts_with_severe_ASR_flags']))+' overlength='+str(len(result['overlength_entries'])))


if __name__=='__main__':main()
