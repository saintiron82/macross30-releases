"""Historical offline Lua experiment, superseded by native post-selection hooks.

Do not install this output: the failure reroute changes original voice selection.
Never changes active game packs.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    output = ROOT / 'localization/subtitles/battle-voice/script-candidate'
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for name in ('common.lua', 'event_general.lua'):
        original = (ROOT / 'analysis/scripts' / name).read_bytes()
        candidate = original
        edits = []
        if name == 'common.lua':
            # Restrict the change to the two battle wrappers. Explicit OFF stays OFF.
            for func in ('BattleSpeak', 'BattleSpeakNW'):
                start = candidate.index(('function ' + func + '(').encode())
                end = candidate.index(b'\nend', start)
                block = candidate[start:end]
                assert block.count(b'str_flag = OFF') == 1
                candidate = candidate[:start] + block.replace(b'str_flag = OFF', b'str_flag = ON') + candidate[end:]
                edits.append(func + ': omitted caption flag defaults ON; explicit OFF preserved')
            before = b'if( str_flag == ON )then\r\n\t\t\tcaption = r_id'
            after = b'if( str_flag == ON and gop_data[r_id].strText ~= "" )then\r\n\t\t\tcaption = r_id'
            assert candidate.count(before) == 1
            candidate = candidate.replace(before, after)
            edits.append('Empty original text does not enqueue an empty caption')
        else:
            start = candidate.index(b'function AFTER_FAIL_EVENT()')
            end = candidate.index(b'\nend', start)
            block = candidate[start:end]
            for line in block.splitlines(keepends=True):
                if b'\tEQVoice(' in line:
                    assert any(token in line for token in (b'101080, 101090, 101100', b'1570, 1580, 1590'))
                    newline = b'\r\n' if line.endswith(b'\r\n') else b'\n'
                    indent = line[:len(line)-len(line.lstrip(b'\t'))]
                    replacement = line.replace(b'EQVoice(', b'BattleSpeak(', 1) + indent + b'DeleteSpeak()' + newline
                    assert block.count(line) == 1
                    block = block.replace(line, replacement)
                    edits.append('Mission-failure voice uses selected battle record caption and clears after blocking playback')
            assert len(edits) == 2
            candidate = candidate[:start] + block + candidate[end:]
        (output / name).write_bytes(candidate)
        manifest.append(dict(file=name, original_sha256=hashlib.sha256(original).hexdigest(),
                             candidate_sha256=hashlib.sha256(candidate).hexdigest(), edits=edits))
    report = dict(status='SUPERSEDED_DO_NOT_INSTALL', installed=False, install_allowed=False, live_game_verified=False,
                  files=manifest,
                  remaining=['Lua execution fixture and caption lifetime/priority verification',
                             'Compare native EQVoice table selection against Lua SelectVoice before deploying failure reroute',
                             'Respect runtime subtitle option for battle captions',
                             'Stage every active Lua pack/index route with rollback',
                             'Voice-only records require original audio identification and transcription'],
                  invariants=['HP triggers and original voice candidate lists unchanged; selection/face side effects still require comparison',
                              'No active game pack, ELF, or running process changes'])
    (output / 'manifest.json').write_bytes((json.dumps(report, ensure_ascii=False, indent=2)+'\n').encode())
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
