"""Translate frozen English sidecar scopes with the installed local engine."""
import argparse
import json
import time
import urllib.request
from english_pack import ROOT,digest,write
from translate_english_local import request,mask,validate,BASE,SYSTEM

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--model',default='qwen3.5:4b');parser.add_argument('--kind',default='all');parser.add_argument('--limit',type=int,default=0)
    args=parser.parse_args()
    models={r['name']:r for r in json.load(urllib.request.urlopen('http://127.0.0.1:11434/api/tags',timeout=10))['models']}
    if args.model not in models:raise ValueError('Only an already installed model may be used')
    scope=BASE/'sidecar-scope.json';source=json.loads(scope.read_text(encoding='utf8'))
    path=BASE/'sidecar-results.json'
    result=json.loads(path.read_text(encoding='utf8')) if path.exists() else dict(scope_sha256=digest(scope.read_bytes()),entries={},rejected={})
    assert result['scope_sha256']==digest(scope.read_bytes())
    jobs=sorted(source['jobs'],key=lambda r:['elf','movie','voice'].index(r['kind']))
    pending=[r for r in jobs if r['id'] not in result['entries'] and args.kind in ('all',r['kind'])]
    started=time.time();done=0
    while pending:
        if (BASE/'sidecars-stop.request').exists():break
        if args.limit and done>=args.limit:break
        rows=pending[:min(64,args.limit-done if args.limit else 64)]
        rows=[r for r in rows if r['kind']==rows[0]['kind']]
        while len(rows)>1 and sum(len(r['source']) for r in rows)>1800:rows=rows[:-1]
        retries=0
        while True:
            try:values,stats=request(rows,args.model);break
            except (ValueError,KeyError,OSError,TimeoutError) as error:
                retries+=1
                if len(rows)>1:rows=rows[:max(1,len(rows)//2)]
                elif retries>=3:values={rows[0]['id']:''};stats=dict(error=str(error));break
        for row in rows:
            text,reason=validate(row['source'],values[row['id']],mask(row['source'])[1])
            if reason:result['rejected'][row['id']]=dict(source=row['source'],candidate=values[row['id']],reason=reason)
            else:
                result['entries'][row['id']]=dict(source=row['source'],translation=text,model=args.model,model_digest=models[args.model]['digest'],prompt_sha256=digest(SYSTEM.encode()),status='local_model_draft_needs_review')
                result['rejected'].pop(row['id'],None)
        write(path,result);done+=len(rows);pending=pending[len(rows):]
        progress=dict(kind=rows[0]['kind'],attempted=done,accepted=len(result['entries']),rejected=len(result['rejected']),seconds=round(time.time()-started),stats=stats)
        write(BASE/'sidecar-progress.json',progress);print(json.dumps(progress),flush=True)
