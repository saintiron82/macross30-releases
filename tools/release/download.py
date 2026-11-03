"""Direct HTTPS Archive ISO download, resumable ranges and metadata hash verification."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import threading
import time
import urllib.parse
import urllib.request


@contextmanager
def output_lock(path):
    with Path(path).open('a+b') as stream:
        stream.seek(0,2)
        if not stream.tell():stream.write(b'0');stream.flush()
        stream.seek(0)
        if sys.platform=='win32':
            import msvcrt
            msvcrt.locking(stream.fileno(),msvcrt.LK_NBLCK,1)
        else:
            import fcntl
            fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
        try:yield
        finally:
            stream.seek(0)
            if sys.platform=='win32':msvcrt.locking(stream.fileno(),msvcrt.LK_UNLCK,1)
            else:fcntl.flock(stream,fcntl.LOCK_UN)


def hashes(path):
    values={name:hashlib.new(name) for name in ('sha1','md5','sha256')}
    with Path(path).open('rb') as stream:
        while data:=stream.read(8<<20):
            for digest in values.values():digest.update(data)
    return {name:digest.hexdigest() for name,digest in values.items()}


def download(item, row, directory, *, workers=8, chunk_size=32<<20, attempts=6,
             opener=urllib.request.urlopen, progress=None, mirrors=()):
    name=row.get('name','')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+',item) or '..' in item:
        raise ValueError('Invalid Archive item identifier')
    if Path(name).name!=name or '/' in name or '\\' in name or ':' in name or not name.lower().endswith('.iso'):
        raise ValueError('Select a single ISO file; torrent and other file types are not downloaded')
    size=int(row['size'])
    if size<=0 or not 1<=workers<=16 or chunk_size<=0 or attempts<=0:raise ValueError('Invalid download parameters')
    expected={key:row[key].lower() for key in ('sha1','md5')}
    if not re.fullmatch('[0-9a-f]{40}',expected['sha1']) or not re.fullmatch('[0-9a-f]{32}',expected['md5']):
        raise ValueError('Archive metadata must provide SHA-1 and MD5')
    directory=Path(directory).resolve();directory.mkdir(parents=True,exist_ok=True)
    final=directory/name;part=directory/(name+'.part');state_file=directory/(name+'.download.json')
    url='https://archive.org/download/'+item+'/'+urllib.parse.quote(name)
    urls=[url]
    for mirror in mirrors:
        parsed=urllib.parse.urlsplit(mirror)
        if parsed.scheme!='https' or not re.fullmatch(r'ia\d+\.(?:us\.)?archive\.org',parsed.netloc):
            raise ValueError('Invalid Archive mirror')
        if not parsed.path.endswith('/items/'+item+'/'+urllib.parse.quote(name)) or parsed.query or parsed.fragment:
            raise ValueError('Mirror does not identify the requested file')
        urls.append(mirror)
    with output_lock(directory/(name+'.lock')):
        if final.exists():
            actual=hashes(final)
            if final.stat().st_size!=size or any(actual[k]!=v for k,v in expected.items()):
                raise ValueError('Existing ISO differs; it was preserved')
            return dict(verified=True,path=str(final),bytes=size,hashes=actual,reused=True)
        metadata=dict(url=url,size=size,chunk_size=chunk_size,expected=expected)
        if state_file.exists():
            state=json.loads(state_file.read_text())
            if any(state.get(k)!=v for k,v in metadata.items()):raise ValueError('Resume metadata differs; partial file preserved')
            if not part.is_file() or part.stat().st_size!=size:raise ValueError('Resume file is missing or has the wrong size')
        else:
            if part.exists():raise ValueError('Untracked partial file preserved')
            if shutil.disk_usage(directory).free<size+(2<<30):raise ValueError('Insufficient free space')
            with part.open('xb') as stream:stream.truncate(size)
            state=dict(**metadata,completed={},status='downloading')
        state['status']='downloading';state.pop('error',None)
        lock=threading.Lock();stop=threading.Event();inflight={}
        def save():
            pending=state_file.with_suffix('.pending')
            pending.write_text(json.dumps(state,indent=2));pending.replace(state_file)
        with part.open('rb') as stream:
            for key,expected_chunk in list(state['completed'].items()):
                index=int(key)
                if index<0 or index*chunk_size>=size:raise ValueError('Invalid resume range index')
                stream.seek(index*chunk_size)
                if hashlib.sha256(stream.read(min(chunk_size,size-index*chunk_size))).hexdigest()!=expected_chunk:
                    del state['completed'][key]
        save()
        def report():
            with lock:
                done=sum(min(chunk_size,size-int(k)*chunk_size) for k in state['completed'])+sum(inflight.values())
                snapshot=dict(status=state['status'],downloaded_bytes=done,total_bytes=size,percent=round(done/size*100,2))
            if progress:progress(snapshot)
        def worker(index):
            begin=index*chunk_size;end=min(begin+chunk_size,size)-1
            total=0;digest=hashlib.sha256()
            for attempt in range(attempts):
                if stop.is_set():raise RuntimeError('Download stopped after another range failed')
                offset=begin+total
                try:
                    request=urllib.request.Request(urls[(index+attempt)%len(urls)],headers={'User-Agent':'Macross30-DirectDownload/1.0','Range':f'bytes={offset}-{end}'})
                    with opener(request,timeout=90) as response:
                        if response.status!=206 or response.headers.get('Content-Range')!=f'bytes {offset}-{end}/{size}':
                            raise ValueError('Server did not honor the requested byte range')
                        with part.open('r+b') as stream:
                            stream.seek(offset)
                            while total<end-begin+1:
                                if stop.is_set():raise RuntimeError('Download stopped')
                                data=response.read(min(1<<20,end-begin+1-total))
                                if not data:raise IOError('Truncated download range')
                                stream.write(data);digest.update(data);total+=len(data)
                                with lock:inflight[index]=total
                            stream.flush()
                    with lock:
                        state['completed'][str(index)]=digest.hexdigest();inflight.pop(index,None);save()
                    return
                except Exception:
                    if attempt+1==attempts:
                        stop.set();raise
                    if stop.wait(min(2**attempt,20)):raise RuntimeError('Download stopped')
        done_event=threading.Event()
        def monitor():
            while not done_event.wait(10):report()
        monitor_thread=threading.Thread(target=monitor,daemon=True);monitor_thread.start()
        try:
            with ThreadPoolExecutor(max_workers=workers) as pool:
                jobs=[pool.submit(worker,i) for i in range((size+chunk_size-1)//chunk_size) if str(i) not in state['completed']]
                try:
                    for job in as_completed(jobs):job.result()
                except Exception:
                    stop.set()
                    for job in jobs:job.cancel()
                    raise
            with lock:state['status']='verifying';save()
            report();actual=hashes(part)
            if any(actual[k]!=v for k,v in expected.items()):
                with lock:state['status']='hash_mismatch';save()
                raise ValueError('Downloaded ISO hash mismatch; partial file preserved')
            part.rename(final)
            result=dict(verified=True,path=str(final),bytes=size,hashes=actual,source=url)
            with lock:state.update(status='verified',result=result);save()
            return result
        except Exception as error:
            with lock:
                if state['status']!='hash_mismatch':state['status']='failed'
                state['error']=type(error).__name__;save()
            raise
        finally:
            done_event.set();monitor_thread.join(timeout=2)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--item',required=True)
    parser.add_argument('--file',required=True)
    parser.add_argument('--output-dir',required=True,type=Path)
    parser.add_argument('--workers',type=int,default=8)
    args=parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+',args.item):parser.error('Invalid item identifier')
    with urllib.request.urlopen('https://archive.org/metadata/'+args.item,timeout=30) as response:metadata=json.load(response)
    row=next((r for r in metadata['files'] if r['name']==args.file),None)
    if row is None:parser.error('The exact requested file is not listed in item metadata')
    mirrors=['https://'+metadata[k]+metadata['dir']+'/'+urllib.parse.quote(args.file)
             for k in ('d1','d2') if metadata.get(k) and metadata.get('dir')]
    result=download(args.item,row,args.output_dir,workers=args.workers,mirrors=mirrors,
                    progress=lambda state:print(json.dumps(state),flush=True))
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
