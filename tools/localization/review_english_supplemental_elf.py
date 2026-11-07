"""Review original display literals outside the Korean parity baseline.

Stages drafts only. Applying requires exact source identity, original slot size,
font/format guards, and a separate reviewed catalog; published ELF is never edited.
"""
import argparse
import json
import time
import urllib.request
from english_pack import ROOT, digest, write
from translate_english_local import request, validate, mask, SYSTEM

BASE = ROOT / 'localization/en-US/review-v10'


def scope():
    original = (ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    current = (ROOT / 'build/english-preview-images-v9/EBOOT.english.elf').read_bytes()
    candidates = json.loads((ROOT / 'analysis/localization/elf_japanese_candidates.json').read_text(encoding='utf8'))['entries']
    rows = []
    extra = {0x9170d8, 0x918720, 0x9193c0, 0x919d18, 0x91c140, 0x91d580, 0x91dd28, 0x91e408, 0x91e978,
             0x920308, 0x9204f0, 0x9206c8, 0x9206f8, 0x9214a8, 0x921668, 0x9216a8,
             0x909cf8, 0x93c168, 0x94d728, 0x94d758, 0x951278}
    for r in candidates:
        off = r['file_offset']; raw = r['source'].encode() + b'\0'
        selected = 0x913378 <= off < 0x914600 or 0x925918 <= off <= 0x92bdf8 or off in extra
        if selected and current[off:off+len(raw)] == original[off:off+len(raw)] == raw:
            rows.append(dict(id=r['id'], offset=off, source=r['source'], capacity=len(raw),
                             role='equipment display' if off < 0x925918 else 'music/pilot gallery display'))
    return dict(original_sha256=digest(original), baseline_sha256=digest(current), entries=rows,
                selection='Explicit reviewed display ranges; internal identifiers and debug diagnostics excluded')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--translate', action='store_true')
    args = parser.parse_args(); BASE.mkdir(parents=True, exist_ok=True)
    doc = scope(); write(BASE / 'supplemental-elf-scope.json', doc)
    print(dict(display_literals=len(doc['entries'])), flush=True)
    if args.translate:
        model = 'qwen3.5:4b'
        models = {r['name']:r for r in json.load(urllib.request.urlopen('http://127.0.0.1:11434/api/tags'))['models']}
        if model not in models:raise ValueError('No installed translation model')
        path = BASE / 'supplemental-elf-drafts.json'
        result = json.loads(path.read_text(encoding='utf8')) if path.exists() else dict(scope_sha256=digest((BASE/'supplemental-elf-scope.json').read_bytes()), entries={}, rejected={})
        assert result['scope_sha256'] == digest((BASE/'supplemental-elf-scope.json').read_bytes())
        pending = [r for r in doc['entries'] if r['id'] not in result['entries']]
        start = time.time()
        while pending:
            rows = pending[:8]
            while len(rows)>1 and sum(len(r['source']) for r in rows)>1000:rows=rows[:-1]
            for attempt in range(3):
                try:values,stats=request(rows,model);break
                except (ValueError,KeyError,OSError,TimeoutError):
                    if len(rows)>1:rows=rows[:max(1,len(rows)//2)]
                    elif attempt==2:raise
            for r in rows:
                text,reason=validate(r['source'],values[r['id']],mask(r['source'])[1])
                if reason:result['rejected'][r['id']]=dict(source=r['source'],candidate=values[r['id']],reason=reason)
                else:
                    result['entries'][r['id']]=dict(source=r['source'],translation=text,model=model,model_digest=models[model]['digest'],prompt_sha256=digest(SYSTEM.encode()),review='pending')
                    result['rejected'].pop(r['id'],None)
            write(path,result);pending=pending[len(rows):]
            print(dict(accepted=len(result['entries']),rejected=len(result['rejected']),remaining=len(pending),seconds=round(time.time()-start)),flush=True)
