"""Compile exact resolved speaker/voice stems into a bounded Korean lookup table."""
import hashlib
import json
from pathlib import Path
import struct
import unicodedata
import re
from caption_text import clean_caption_text, has_work_annotation
from audit_runtime_font import read_font
from audit_unresolved_voice_translations import SEVERE_FLAGS

ROOT=Path(__file__).resolve().parents[2]
mapping_path=ROOT/'localization/subtitles/battle-voice/record-voice-text-map.json'
translations_path=ROOT/'localization/ko-KR/structured/gop_battletext.json'
mapping=json.loads(mapping_path.read_text(encoding='utf8'))
catalog=json.loads(translations_path.read_text(encoding='utf8'))
original=ROOT/catalog['source_file']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(original)==catalog['source_sha256']==mapping['source_sha256']
by_index={r['index']:r for r in catalog['entries']}
entries={}
untexted=[]
for record in mapping['records']:
    assert record['record_id'].startswith('R_BattleText')
    stem=record['record_id'][len('R_BattleText'):]
    speaker,voice=stem.rsplit('_',1)
    assert voice==record['voice_key'] and len(voice)==6 and voice.isascii() and voice.isdigit()
    assert speaker and all(c in 'abcdefghijklmnopqrstuvwxyz0123456789_' for c in speaker)
    key=speaker+'/'+voice
    translated=by_index.get(record['text_string_index'])
    if translated:
        assert translated['source']==record['source']
        text=translated['translation']
    elif record['source']=='OK':
        text='OK'
    else:
        assert not record['source'],'Unexpected untranslated original text'
        untexted.append(record['record_id'])
        continue
    assert text and '\x00' not in text and len(text.encode('utf8'))<=254
    assert key not in entries,'Ambiguous resolved voice stem'
    entries[key]=text
battle_count=len(entries)
audit=ROOT/'analysis/localization/voice-only-audit'
scenario_path=audit/'scenario-voice-map.json'
scenario=json.loads(scenario_path.read_text(encoding='utf8'))
scenario_catalog_path=ROOT/'localization/ko-KR/structured/gop_scenariotext.json'
scenario_catalog=json.loads(scenario_catalog_path.read_text(encoding='utf8'))
assert sha(ROOT/scenario['source'])==scenario['source_sha256']==scenario_catalog['source_sha256']
assert sha(scenario_catalog_path)==scenario['translation_catalog_sha256']
scenario_ko={r['index']:r for r in scenario_catalog['entries']}
elf_path=ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
elf=elf_path.read_bytes()
assert sha(elf_path)=='44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'
def resolved_speaker(character):
    assert 0<=character<191
    pos=0xB5A330+character*0xD8
    face,voice=struct.unpack_from('>2I',elf,pos+0x50)
    chosen=voice if voice!=191 else face if face!=191 else character
    assert 0<=chosen<191
    pointer=struct.unpack_from('>I',elf,0xB5A330+chosen*0xD8+0x44)[0]
    end=elf.index(b'\0',pointer-0x10000)
    return elf[pointer-0x10000:end].decode('ascii')
collisions=[]
for record in scenario['records']:
    if not record['voice_key']:continue
    key=resolved_speaker(record['character_enum'])+'/'+record['voice_key']
    assert key==record['resolved_stem']
    translated=scenario_ko.get(record['text_string_index'])
    if translated:
        assert translated['source']==record['source']
        text=translated['translation']
    else:
        # Nonverbal punctuation/countdown entries intentionally need no Japanese
        # translation entry; normalize fullwidth ASCII without inventing words.
        import unicodedata
        text=unicodedata.normalize('NFKC',record['source'])
        assert all(not ('\u3040'<=c<='\u30ff' or '\u3400'<=c<='\u9fff') for c in text)
    assert text and '\x00' not in text and len(text.encode())<=254
    if key in entries:
        if entries[key]!=text:collisions.append(dict(stem=key,battle_preferred=entries[key],scenario_variant=text))
    else:entries[key]=text
scenario_added=len(entries)-battle_count
additional_tables=[]
for name in ('gop_subquesttext','gop_systemtext'):
    map_path=audit/(name+'-voice-map.json')
    data=json.loads(map_path.read_text(encoding='utf8'))
    ko_path=ROOT/data['translation_catalog']
    ko_data=json.loads(ko_path.read_text(encoding='utf8'))
    assert sha(ROOT/data['source'])==data['source_sha256']==ko_data['source_sha256']
    assert sha(ko_path)==data['translation_catalog_sha256']
    lookup={r['index']:r for r in ko_data['entries']}
    added=0
    for row in data['records']:
        if not row['voice_key']:continue
        key=resolved_speaker(row['character_enum'])+'/'+row['voice_key']
        assert key==row['resolved_stem']
        translated=lookup[row['text_string_index']]
        assert translated['source']==row['source']
        text=translated['translation']
        assert text and '\x00' not in text and len(text.encode())<=254
        if key not in entries:entries[key]=text;added+=1
        elif entries[key]!=text:collisions.append(dict(stem=key,previous_preferred=entries[key],table=name,variant=text))
    additional_tables.append(dict(name=name,added=added,map_sha256=sha(map_path),translation_catalog_sha256=sha(ko_path)))
aliases_path=audit/'exact-alias-caption-map-all-tables.json'
aliases=json.loads(aliases_path.read_text(encoding='utf8'))
assert aliases['candidate_count']==253 and aliases['all_unique_Korean_text']
assets={a['full_path']:a for a in json.loads((audit/'actual-voice-assets.json').read_text(encoding='utf8'))['assets']}
checked={}
pack=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
def exact_audio(path,expected):
    if path not in checked:
        row=assets[path]
        with (pack/row['pack']).open('rb') as f:f.seek(row['offset']);data=f.read(row['size'])
        assert len(data)==row['size'] and data[:4]==b'RIFF'
        checked[path]=hashlib.sha256(data).hexdigest()
    assert checked[path]==expected==assets[path]['sha256'],'Exact audio alias changed'
for row in aliases['entries']:
    assert row['resolved_stem'] not in entries
    exact_audio(row['target_path'],row['sha256'])
    assert row['matching_text_asset_paths']
    for source in row['matching_text_asset_paths']:exact_audio(source,row['sha256'])
    assert row['translation'] and len(row['translation'].encode())<=254
    entries[row['resolved_stem']]=row['translation']
original_stems=len(entries)
assert original_stems==14365
# Add only the accepted Small-ASR drafts. Medium review candidates remain held.
draft_path=ROOT/'localization/subtitles/battle-voice/asr-caption-drafts.json'
drafts=json.loads(draft_path.read_text(encoding='utf8'))
assert drafts['status']=='MECHANICALLY_CHECKED_UNREVIEWED_ASR_DRAFTS'
assert drafts['accepted_unique_audio']==231 and drafts['accepted_paths']==238
assert drafts['held_unique_audio']==182 and drafts['human_listening_verified'] is False
assert drafts['required_font_bundle']=='battle-voice-font-v13'
font_bundle=ROOT/'localization/image_assets'/drafts['required_font_bundle']
assert json.loads((font_bundle/'candidate-verification.json').read_text(encoding='utf8'))['status']=='PASS'
authority=json.loads((font_bundle/'candidate/authority.json').read_text(encoding='utf8'))
font=next(r for r in authority['entries'] if r['type']=='ABF')
assert sha(Path(font['replacement']))==font['replacement_sha256']==drafts['required_font_ABF_sha256']
_,glyphs,_=read_font(Path(font['replacement']).read_bytes())
font_codepoints={r['codepoint'] for r in glyphs}
assert len(font_codepoints)==8177
accepted_by_sha={}
for batch in drafts['batches']:
    number=batch['batch']
    source=ROOT/f'localization/subtitles/battle-voice/translation-batches/batch-{number}.json'
    translated=source.with_name(f'batch-{number}.ko.json')
    assert sha(source)==batch['input_sha256'] and sha(translated)==batch['output_sha256']
    input_doc=json.loads(source.read_text(encoding='utf8'))
    output_doc=json.loads(translated.read_text(encoding='utf8'))
    assert output_doc['batch']==number and output_doc['input_sha256']==sha(source)
    assert len(input_doc['entries'])==len(output_doc['entries'])
    for original_row,translated_row in zip(input_doc['entries'],output_doc['entries']):
        assert all(translated_row[k]==v for k,v in original_row.items()),'ASR source provenance changed'
        if translated_row['status']=='draft':
            key=original_row['source_sha256']
            assert key not in accepted_by_sha
            accepted_by_sha[key]=(number,original_row,translated_row)
assert {r['batch'] for r in drafts['batches']}=={1,2,3}
draft_seen=set()
for row in drafts['entries']:
    assert row['status']=='unreviewed_ASR_translation_draft' and row['human_listening_verified'] is False
    number,source_row,translated_row=accepted_by_sha[row['source_sha256']]
    assert row['batch']==number and row['source_ja']==source_row['source_ja']
    assert row['source_path'] in source_row['source_paths']
    assert row['translation']==translated_row['translation_ko']
    assert not SEVERE_FLAGS.intersection(source_row['review_flags'])
    assert row['source_path']=='data/sound/voice/'+row['stem']+'.at3'
    assert row['stem'] not in draft_seen
    draft_seen.add(row['stem'])
    exact_audio(row['source_path'],row['source_sha256'])
    text=clean_caption_text(row['translation'])
    assert text and '\x00' not in text and len(text.encode('utf8'))<=254
    assert not has_work_annotation(text)
    assert all(ord(c) in font_codepoints for c in text if c not in '\n\r\t')
    assert row['stem'] not in entries,'ASR must never replace an original caption'
    entries[row['stem']]=text
assert len(draft_seen)==238 and len({r['source_sha256'] for r in drafts['entries']})==231
assert len(entries)==14603
medium_base=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1'
review_base=medium_base/'candidate-review-v1'
candidate_path=review_base/'candidates.json'
reviewed_path=review_base/'translation-reviewed.json'
reviewed=json.loads(reviewed_path.read_text(encoding='utf8'))
assert reviewed['status']=='TRANSLATION_REVIEWED_DUAL_ASR_DRAFTS_NOT_DEPLOYED'
assert sha(candidate_path)==reviewed['source_sha256']=='2cb714f811a71062e18a335996cd98a039ac923e7e0d9c73e04ca0d7f857dc52'
assert reviewed['human_listening_verified'] is False
candidate_by_index={r['index']:r for r in json.loads(candidate_path.read_text(encoding='utf8'))['entries']}
medium_plan=json.loads((medium_base/'plan.json').read_text(encoding='utf8'))
plan_by_index={r['priority_index']:r for r in medium_plan['entries']}
def meaningful_key(text):
    return ''.join(c for c in unicodedata.normalize('NFKC',text) if not c.isspace() and unicodedata.category(c)[0] not in 'PS')
medium_seen=set()
for row in reviewed['entries']:
    source_row=candidate_by_index[row['index']]
    assert all(row[k]==v for k,v in source_row.items() if k not in ('status','publication_authorized','translation_ko'))
    assert row['publication_authorized'] is True and row['status']=='translation_reviewed_dual_ASR_draft'
    assert row['human_listening_verified'] is False
    frozen=plan_by_index[row['index']]
    assert row['source_sha256']==frozen['source_sha256'] and row['source_paths']==frozen['source_paths']
    assert row['Small_source_ja']==frozen['Small_source_ja'] and row['audio_sha256']==frozen['audio_sha256']
    assert row['Small_translation_input_sha256']==frozen['batch_input']['Small_translation_input_sha256']
    assert row['initial_KO_draft_sha256']==frozen['batch_input']['initial_KO_draft_sha256']
    assert sha(ROOT/frozen['batch_input']['Small_translation_input'])==row['Small_translation_input_sha256']
    assert sha(ROOT/frozen['Small_transcript'])==frozen['Small_transcript_sha256']
    assert sha(ROOT/row['audio'])==row['audio_sha256']
    small=json.loads((ROOT/frozen['Small_transcript']).read_text(encoding='utf8'))
    checkpoint=medium_base/row['checkpoint']
    assert sha(checkpoint)==row['checkpoint_sha256']
    medium=json.loads(checkpoint.read_text(encoding='utf8'))
    assert medium['status']=='completed_unreviewed_Medium_crosscheck'
    assert medium['Small_source_ja']==frozen['Small_source_ja']
    assert small['original_AT3_sha256']==medium['source_sha256']==row['source_sha256']
    assert small['audio_sha256']==medium['audio_sha256']==row['audio_sha256']
    assert ' '.join(c['source_ja'] for c in small['segments'])==row['Small_source_ja']
    assert ' '.join(c['source_ja'] for c in medium['segments'])==medium['Medium_source_ja']==row['Medium_source_ja']
    assert medium['model']['model_bin_sha256']==medium_plan['model']['model_bin_sha256']
    assert medium['parameters']['initial_prompt'] is None and medium['parameters']['condition_on_previous_text'] is False
    assert meaningful_key(row['Small_source_ja'])==meaningful_key(row['Medium_source_ja']) and meaningful_key(row['Small_source_ja'])
    assert medium['comparison']==row['comparison'] and medium['lexical_normalized_match'] is True
    for result in (small,medium):
        flags={f for c in result['segments'] for f in c['review_flags']}
        assert not flags.intersection({'low_average_log_probability','possible_non_speech','possible_generic_ASR_hallucination','very_short_reaction_requires_listening'})
    text=clean_caption_text(row['translation_ko'])
    assert text and not has_work_annotation(text) and '\x00' not in text and len(text.encode('utf8'))<=254
    assert all(ord(c) in font_codepoints for c in text if c not in '\n\r\t')
    assert row['resolved_stems']==[p[len('data/sound/voice/'):-4] for p in row['source_paths']]
    for stem,path in zip(row['resolved_stems'],row['source_paths']):
        assert path=='data/sound/voice/'+stem+'.at3'
        exact_audio(path,row['source_sha256'])
        assert stem not in entries and stem not in medium_seen,'Reviewed ASR cannot replace an earlier caption'
        medium_seen.add(stem);entries[stem]=text
assert len(reviewed['entries'])==len(candidate_by_index)==len(medium_seen)==13
assert len({r['source_sha256'] for r in reviewed['entries']})==13
assert len(entries)==14616
lexical_batches=[]
for version,candidate_sha,expected_clips,expected_paths in (
    ('lexical-review-v2','055feb93fd8492c13dad8e6ea74261a3203c9568e04e9f97bb2bfd1366ee08e2',20,20),
    ('lexical-review-v3','8e239b504f12bd581bd712c1bd5c54a3b43b7818957f4eb2ef03b3fe8bc422a7',18,19),
    ('lexical-review-v4','1064a8e041d77ce7a79f179963767c91101b7195f70df332fc7aac0707e92034',17,19),
    ('lexical-review-v5','87b0e523b3b383ea3a30d11edcf62531846f0231ca4576e6601a49ccb84c4c55',15,15),
    ('lexical-review-v6','fdb6f65a1d60d393fba15437fae1bdba9ff97e9f88b2616b06f2bc8e6e3e14c8',8,8)):
    lexical_base=medium_base/version
    lexical_source_path=lexical_base/'candidates.json'
    lexical_reviewed_path=lexical_base/'translation-reviewed.json'
    lexical_source=json.loads(lexical_source_path.read_text(encoding='utf8'))
    lexical_reviewed=json.loads(lexical_reviewed_path.read_text(encoding='utf8'))
    assert sha(lexical_source_path)==lexical_reviewed['source_sha256']==candidate_sha
    assert lexical_reviewed['status']=='TRANSLATION_REVIEWED_LEXICAL_ASR_DRAFTS_NOT_DEPLOYED'
    assert lexical_reviewed['human_listening_verified'] is False
    assert sha(medium_base/'plan.json')==lexical_source['source_plan_sha256']
    assert sha(medium_base/'review-index.json')==lexical_source['source_review_index_sha256']
    for source_map in lexical_source['source_maps']:
        path=ROOT/source_map['path']
        data=json.loads(path.read_text(encoding='utf8'))
        assert sha(path)==source_map['sha256'] and sha(ROOT/data['source'])==source_map['original_source_sha256']
    lexical_candidates={r['index']:r for r in lexical_source['entries']}
    lexical_seen=set()
    for row in lexical_reviewed['entries']:
        source_row=lexical_candidates[row['index']]
        assert all(row[k]==v for k,v in source_row.items() if k not in ('status','publication_authorized'))
        assert row['status']=='translation_reviewed_lexical_ASR_draft' and row['publication_authorized'] is True
        assert row['human_listening_verified'] is False and row['reviewer_normalized_ja']
        if 'name_source_proof' in row:
            proof=row['name_source_proof'];name_path=ROOT/proof['dictionary_path']
            names=json.loads(name_path.read_text(encoding='utf8'))
            assert sha(name_path)==proof['dictionary_sha256']
            assert sha(ROOT/names['source_file'])==names['source_sha256']==proof['dictionary_original_source_sha256']
            by_name_index={r['index']:r for r in names['entries']}
            assert all(by_name_index[r['index']]==r for r in proof['focker_entries'])
        frozen=plan_by_index[row['index']]
        assert row['source_sha256']==frozen['source_sha256'] and row['source_paths']==frozen['source_paths']
        assert row['Small_source_ja']==frozen['Small_source_ja']
        assert row['frozen_input_sha256']==sha(ROOT/frozen['batch_input']['Small_translation_input'])==frozen['batch_input']['Small_translation_input_sha256']
        assert row['initial_KO_draft_sha256']==frozen['batch_input']['initial_KO_draft_sha256']
        assert row['Small_transcript']==frozen['Small_transcript']
        assert sha(ROOT/row['Small_transcript'])==row['Small_transcript_sha256']==frozen['Small_transcript_sha256']
        assert row['audio']==frozen['audio'] and sha(ROOT/row['audio'])==row['audio_sha256']==frozen['audio_sha256']
        checkpoint=medium_base/row['checkpoint'];assert sha(checkpoint)==row['checkpoint_sha256']
        small=json.loads((ROOT/row['Small_transcript']).read_text(encoding='utf8'))
        medium=json.loads(checkpoint.read_text(encoding='utf8'))
        assert medium['status']=='completed_unreviewed_Medium_crosscheck'
        assert medium['model']['model_bin_sha256']==medium_plan['model']['model_bin_sha256']
        assert medium['parameters']['initial_prompt'] is None and medium['parameters']['condition_on_previous_text'] is False
        assert small['original_AT3_sha256']==medium['source_sha256']==row['source_sha256']
        assert small['audio_sha256']==medium['audio_sha256']==row['audio_sha256']
        assert ' '.join(c['source_ja'] for c in small['segments'])==medium['Small_source_ja']==row['Small_source_ja']
        assert ' '.join(c['source_ja'] for c in medium['segments'])==medium['Medium_source_ja']==row['Medium_source_ja']
        text=clean_caption_text(row['translation_ko'])
        assert text and not has_work_annotation(text) and '\x00' not in text and len(text.encode())<=254
        assert all(ord(c) in font_codepoints for c in text if c not in '\n\r\t')
        assert row['resolved_stems']==[p[len('data/sound/voice/'):-4] for p in row['source_paths']]
        for stem,path in zip(row['resolved_stems'],row['source_paths']):
            assert path=='data/sound/voice/'+stem+'.at3'
            exact_audio(path,row['source_sha256'])
            assert stem not in entries and stem not in lexical_seen,'Lexical drafts cannot replace previous captions'
            entries[stem]=text;lexical_seen.add(stem)
    assert len(lexical_seen)==expected_paths and len(lexical_candidates)==len(lexical_reviewed['entries'])==expected_clips
    assert len({r['source_sha256'] for r in lexical_reviewed['entries']})==expected_clips

    lexical_batches.append(dict(version=version,paths_added=len(lexical_seen),unique_audio=expected_clips,reviewed_sha256=sha(lexical_reviewed_path),candidate_source_sha256=sha(lexical_source_path)))
assert len(entries)==14697
linebreak_path=ROOT/'localization/subtitles/battle-voice/battle-caption-linebreaks-v1.json'
linebreak_metadata=dict(applied_stems=0,approved_manifest_present=False,live_game_verified=False)
if linebreak_path.exists():
    approved_breaks=json.loads(linebreak_path.read_text(encoding='utf8'))
    assert approved_breaks['status']=='NATIVE_RENDER_REVIEWED_LINEBREAKS'
    assert approved_breaks['source_font_sha256']==drafts['required_font_ABF_sha256']
    proposal_path=ROOT/'analysis/localization/battle-caption-linebreak-candidates-600px-v1.json'
    assert sha(proposal_path)==approved_breaks['source_proposal_sha256']=='d55381da6477aa2b745826236328f719f49264902528c0357953a1dc7cda4d85'
    proposal=json.loads(proposal_path.read_text(encoding='utf8'))
    raw_lines=['// Generated by tools/localization/build_battle_voice_catalog.py. UTF-8.']
    raw_lines.extend('    {'+json.dumps(k)+', '+json.dumps(v,ensure_ascii=False)+'},' for k,v in sorted(entries.items()))
    raw_catalog_sha=hashlib.sha256(('\n'.join(raw_lines)+'\n').encode('utf8')).hexdigest()
    assert raw_catalog_sha==approved_breaks['source_catalog_sha256']==proposal['source_catalog_sha256']
    proof=approved_breaks['render_proof']
    assert proof['root_reviewed'] is True
    proof_path=(ROOT/proof['path']).resolve()
    assert proof_path.is_relative_to(ROOT) and proof_path.is_file() and sha(proof_path)==proof['sha256']
    proposed={r['stem']:r for r in proposal['entries']}
    approved_rows=approved_breaks['entries']
    assert len(approved_rows)==len(proposed)==19
    assert {r['stem'] for r in approved_rows}==set(proposed)
    glyph_by_code={r['codepoint']:r for r in glyphs}
    before_formatting=dict(entries);width_checks=[]
    printf=re.compile(r'%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]')
    for row in approved_rows:
        stem=row['stem'];original=row['original_text'];candidate=row['candidate_text']
        assert original==entries[stem]==proposed[stem]['original_text']
        assert candidate==proposed[stem]['candidate_text']
        assert '\n' not in original and '\r' not in original and candidate.count('\n')==1
        assert candidate.replace('\n',' ')==original and len(candidate.encode())==len(original.encode())
        assert ''.join(candidate.split())==''.join(original.split()) and printf.findall(original)==printf.findall(candidate)
        measured=[]
        for part in candidate.split('\n'):
            assert part and part==part.strip()
            cursor=0;left=0;right=0
            for char in part:
                g=glyph_by_code[ord(char)]
                left=min(left,cursor+g['bearing_x']);right=max(right,cursor+g['bearing_x']+g['width']);cursor+=g['advance']
            cursor_px=cursor*28/24;ink_px=(right-left)*28/24
            assert max(cursor_px,ink_px)<=600
            measured.append(dict(cursor_px=cursor_px,ink_width_px=ink_px))
        entries[stem]=candidate;width_checks.append(dict(stem=stem,lines=measured))
    assert {k for k in entries if entries[k]!=before_formatting[k]}==set(proposed)
    linebreak_metadata=dict(applied_stems=19,approved_manifest_present=True,manifest_sha256=sha(linebreak_path),
        source_catalog_sha256=raw_catalog_sha,source_proposal_sha256=sha(proposal_path),
        render_proof_sha256=proof['sha256'],root_render_review_declared=True,
        ASCII_space_to_LF_only=True,UTF8_bytecount_preserved=True,font_scale='28/24',line_width_budget_px=600,
        width_checks=width_checks,other_stems_unchanged=True,live_game_verified=False)
lines=['// Generated by tools/localization/build_battle_voice_catalog.py. UTF-8.']
for key,value in sorted(entries.items()):
    lines.append('    {'+json.dumps(key)+', '+json.dumps(value,ensure_ascii=False)+'},')
target=ROOT/'native/macross30/battle_voice_catalog.inc'
target.write_bytes(('\n'.join(lines)+'\n').encode('utf8'))
out=ROOT/'localization/subtitles/battle-voice/catalog-build.json'
out.write_text(json.dumps(dict(status='PASS',resolved_stems=len(entries),original_empty_battle_definitions=len(untexted),
    battle_stems=battle_count,scenario_added=scenario_added,exact_audio_aliases_added=len(aliases['entries']),
    additional_tables=additional_tables,
    original_source_stems=original_stems,ASR_draft_stems_added=len(draft_seen),
    ASR_draft_unique_audio=231,ASR_drafts_sha256=sha(draft_path),
    ASR_draft_batches=drafts['batches'],required_font_ABF_sha256=drafts['required_font_ABF_sha256'],
    human_listening_verified=False,Medium_candidates_included=True,
    Medium_translation_reviewed_stems_added=len(medium_seen),Medium_unique_audio=13,
    Medium_reviewed_sha256=sha(reviewed_path),Medium_candidate_source_sha256=sha(candidate_path),
    lexical_reviewed_stems_added=sum(r['paths_added'] for r in lexical_batches),lexical_unique_audio=78,
    lexical_batches=lexical_batches,
    lexical_reviewed_sha256=sha(lexical_reviewed_path),lexical_candidate_source_sha256=sha(lexical_source_path),
    remaining_ASR_held_unique_audio=91,
    approved_linebreaks=linebreak_metadata,
    exact_audio_payloads_rehashed=len(checked),battle_priority_collisions=collisions,
    maximum_UTF8_bytes=max(len(s.encode()) for s in entries.values()),
    original_sha256=sha(original),translation_catalog_sha256=sha(translations_path),
    record_map_sha256=sha(mapping_path),generated_sha256=sha(target),
    original_empty_battle_record_ids=untexted,scenario_map_sha256=sha(scenario_path),
    alias_map_sha256=sha(aliases_path),live_game_verified=False),indent=2)+'\n',encoding='utf8')
print(f"PASS {len(entries)} exact stems: {original_stems} original + {len(draft_seen)} Small ASR + {len(medium_seen)} dual-ASR + {sum(r['paths_added'] for r in lexical_batches)} lexical-reviewed draft stems")
