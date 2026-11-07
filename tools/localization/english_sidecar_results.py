"""Read source-bound model sidecars plus explicit reviewed corrections."""
import json
from english_pack import ROOT,digest

def load_results():
    base=ROOT/'localization/en-US/model-drafts';scope_path=base/'sidecar-scope.json'
    scope=json.loads(scope_path.read_text(encoding='utf8'))
    result=json.loads((base/'sidecar-results.json').read_text(encoding='utf8'))
    assert result['scope_sha256']==digest(scope_path.read_bytes())
    corrections=json.loads((ROOT/'tools/localization/catalogs/sidecar-reviewed-corrections.json').read_text(encoding='utf8'))
    assert corrections['scope_sha256']==result['scope_sha256']
    jobs={r['id']:r for r in scope['jobs']}
    for key,text in corrections['translations'].items():
        assert key in jobs
        result['entries'][key]=dict(source=jobs[key]['source'],translation=text,model='reviewed correction',status='translation_reviewed')
        result['rejected'].pop(key,None)
    return scope,result
