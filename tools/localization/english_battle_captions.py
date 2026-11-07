"""Resolve English battle drafts against original speaker/voice record identities."""
import json
from english_pack import ROOT, digest, write


def main():
    base = ROOT / 'localization/en-US'
    mapping = json.loads((ROOT / 'localization/subtitles/battle-voice/record-voice-text-map.json').read_text(encoding='utf-8'))
    catalog = json.loads((base / 'structured/gop_battletext.json').read_text(encoding='utf-8'))
    if digest((ROOT / mapping['source']).read_bytes()) != mapping['source_sha256'] or mapping['source_sha256'] != catalog['source_sha256']:
        raise ValueError('Battle source mismatch')
    by_index = {r['index']: r for r in catalog['entries']}
    entries, pending, keys = [], [], set()
    for record in mapping['records']:
        if not record['record_id'].startswith('R_BattleText'):
            raise ValueError('Unexpected battle record')
        speaker, voice = record['record_id'][len('R_BattleText'):].rsplit('_', 1)
        if voice != record['voice_key'] or len(voice) != 6 or not voice.isascii() or not voice.isdigit():
            raise ValueError('Voice identity mismatch')
        key = speaker + '/' + voice
        if key in keys:
            raise ValueError('Duplicate speaker/voice identity')
        keys.add(key)
        translated = by_index.get(record['text_string_index'])
        if translated and translated['source'] != record['source']:
            raise ValueError('Mapped source text mismatch')
        text = translated['translation'] if translated else 'OK' if record['source'] == 'OK' else ''
        if text:
            entries.append(dict(key=key, record_id=record['record_id'], source=record['source'],
                                translation_en=text, audio_verified=False))
        else:
            pending.append(dict(key=key, record_id=record['record_id'],
                                string_index=record['text_string_index'], has_original_text=bool(record['source'])))
    result = dict(locale='en-US', status='DRAFT_NOT_INSTALLED', entries=entries, pending=pending,
                  translated=len(entries), total=len(keys),
                  limitations=['Scenario voices and voice-only ASR records require separate catalogs.',
                               'Runtime catalog compilation and in-game rendering remain pending.'])
    write(base / 'subtitles/battle-captions.json', result)
    print(json.dumps({k: result[k] for k in ('locale', 'status', 'translated', 'total')}))


if __name__ == '__main__':
    main()
