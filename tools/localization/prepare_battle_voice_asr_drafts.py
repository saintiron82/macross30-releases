"""Prepare mechanically checked ASR draft captions, keeping uncertain audio held."""
import hashlib
import json
from pathlib import Path

from audit_runtime_font import read_font
from audit_unresolved_voice_translations import SEVERE_FLAGS

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/subtitles/battle-voice'
load=lambda p:json.loads(p.read_text(encoding='utf8'))
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
font_bundle=ROOT/'localization/image_assets/battle-voice-font-v13'
assert load(font_bundle/'candidate-verification.json')['status']=='PASS'
authority=load(font_bundle/'candidate/authority.json')
font=next(r for r in authority['entries'] if r['type']=='ABF')
font_path=Path(font['replacement']);assert sha(font_path)==font['replacement_sha256']
_,glyphs,_=read_font(font_path.read_bytes());codepoints={r['codepoint'] for r in glyphs}
assert len(codepoints)==8177
assets={r['full_path']:r for r in load(ROOT/'analysis/localization/voice-only-audit/actual-voice-assets.json')['assets']}
pack=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
accepted=[];held=[];batches=[];seen=set()
for number in (1,2,3):
    source=B/f'translation-batches/batch-{number}.json'
    translated=B/f'translation-batches/batch-{number}.ko.json'
    original=load(source);doc=load(translated)
    assert doc['input_sha256']==sha(source) and doc['batch']==number
    assert len(original['entries'])==len(doc['entries'])
    for row,output in zip(original['entries'],doc['entries']):
        for key,value in row.items():assert output[key]==value,'Original ASR context changed'
        key=row['source_sha256'];assert key not in seen;seen.add(key)
        text=output['translation_ko']
        flags=SEVERE_FLAGS.intersection(row['review_flags'])
        if output['status']!='draft' or flags or not text:
            held.append(dict(source_sha256=key,source_paths=row['source_paths'],status=output['status'],severe_flags=sorted(flags)))
            continue
        assert '\x00' not in text and len(text.encode())<=254
        assert all(ord(c) in codepoints for c in text if c not in '\n\r\t')
        for path in row['source_paths']:
            asset=assets[path];assert asset['sha256']==key
            with (pack/asset['pack']).open('rb') as f:f.seek(asset['offset']);raw=f.read(asset['size'])
            assert len(raw)==asset['size'] and hashlib.sha256(raw).hexdigest()==key
            assert path.startswith('data/sound/voice/') and path.endswith('.at3')
            stem=path[len('data/sound/voice/'):-4]
            accepted.append(dict(stem=stem,translation=text,source_ja=row['source_ja'],source_sha256=key,
                source_path=path,status='unreviewed_ASR_translation_draft',batch=number,
                native_pilot_IDSTR_present=row['native_speaker_IDSTR_present'],
                human_listening_verified=False))
    batches.append(dict(batch=number,input_sha256=sha(source),output_sha256=sha(translated)))
assert len(seen)==413 and len(held)==182
assert len({r['stem'] for r in accepted})==len(accepted)
doc=dict(status='MECHANICALLY_CHECKED_UNREVIEWED_ASR_DRAFTS',entries=accepted,held=held,batches=batches,
    accepted_unique_audio=len({r['source_sha256'] for r in accepted}),accepted_paths=len(accepted),
    held_unique_audio=len(held),required_font_bundle='battle-voice-font-v13',
    required_font_ABF_sha256=font['replacement_sha256'],human_listening_verified=False,installed=False,
    limits=['Unflagged machine transcripts can still be wrong.',
            'A file existing or matching a native speaker does not prove runtime reachability.',
            'Held audio is not replaced with fabricated text.'])
(B/'asr-caption-drafts.json').write_bytes((json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
print(f'PASS {doc["accepted_unique_audio"]} draft clips / {len(accepted)} exact paths, {len(held)} held; 8177-glyph font required')
