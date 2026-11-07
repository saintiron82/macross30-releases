"""Find possible adjacent-line translation swaps using a local embedding function.

No corrections are automatic. Similarity is a review aid, never semantic proof.
Only an already installed model is used; cached vectors retain model identity.
"""
import argparse
import json
import sqlite3
import time
import urllib.request
import numpy as np
from english_pack import ROOT,digest,write

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--catalog',default='gop_scenariotext');parser.add_argument('--batch-size',type=int,default=32);args=parser.parse_args()
    base=ROOT/'localization/en-US';model='qwen3-embedding:0.6b'
    models={r['name']:r for r in json.load(urllib.request.urlopen('http://127.0.0.1:11434/api/tags'))['models']}
    if model not in models:raise ValueError('Required local embedding model is not installed; no downloads')
    doc=json.loads((base/'structured'/f'{args.catalog}.json').read_text(encoding='utf8'))
    rows=[r for r in doc['entries'] if r['translation']]
    cache=sqlite3.connect(base/'model-drafts/alignment-vectors.sqlite3')
    cache.execute('CREATE TABLE IF NOT EXISTS vectors (key TEXT PRIMARY KEY, value BLOB NOT NULL)')
    def normalize(t):return ' '.join(t.replace(chr(92)+'n',' ').split())
    texts=list(dict.fromkeys(normalize(r[k]) for r in rows for k in ('source','translation')))
    keys={t:digest((models[model]['digest']+'\0'+t).encode()) for t in texts};vectors={};pending=[]
    for t in texts:
        old=cache.execute('SELECT value FROM vectors WHERE key=?',(keys[t],)).fetchone()
        if old:vectors[t]=np.frombuffer(old[0],dtype=np.float32)
        else:pending.append(t)
    started=time.time()
    for at in range(0,len(pending),args.batch_size):
        batch=pending[at:at+args.batch_size]
        payload=dict(model=model,input=batch,truncate=False,keep_alive='30m',options=dict(num_gpu=0,num_thread=4,num_ctx=2048))
        request=urllib.request.Request('http://127.0.0.1:11434/api/embed',data=json.dumps(payload).encode(),headers={'Content-Type':'application/json'})
        response=json.load(urllib.request.urlopen(request,timeout=600))
        if len(response['embeddings'])!=len(batch):raise ValueError('Embedding response identity count changed')
        for text,value in zip(batch,response['embeddings']):
            v=np.asarray(value,dtype=np.float32);assert v.ndim==1 and np.all(np.isfinite(v)) and np.linalg.norm(v)>0
            v=v/np.linalg.norm(v);vectors[text]=v;cache.execute('INSERT OR REPLACE INTO vectors VALUES (?,?)',(keys[text],v.tobytes()))
        cache.commit()
        if at%128==0:print(json.dumps(dict(catalog=args.catalog,embedded=min(at+len(batch),len(pending)),pending=len(pending),seconds=round(time.time()-started))),flush=True)
    flags=[]
    for i,row in enumerate(rows):
        source=normalize(row['source']);target=normalize(row['translation'])
        if len(source)<10:continue
        source_v=vectors[source];own=float(source_v@vectors[target]);lo=max(0,i-8);hi=min(len(rows),i+9)
        scored=[(float(source_v@vectors[normalize(rows[j]['translation'])]),j) for j in range(lo,hi)]
        best,j=max(scored)
        if j!=i and best>0.6 and best-own>0.16:
            flags.append(dict(id=row['id'],source=row['source'],translation=row['translation'],own_similarity=own,neighbor_similarity=best,neighbor_id=rows[j]['id'],neighbor_source=rows[j]['source'],neighbor_translation=rows[j]['translation']))
    write(base/'build'/f'alignment-{args.catalog}.json',dict(model=model,model_digest=models[model]['digest'],catalog=args.catalog,checked=len(rows),flags=flags,heuristics_only=True,semantic_review_complete=False))
    print(json.dumps(dict(catalog=args.catalog,checked=len(rows),flags=len(flags),seconds=round(time.time()-started))),flush=True)
