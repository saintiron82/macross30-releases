"""Explicit, anonymous submission of a pinned and sanitized last-play ZIP."""
import base64
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import tempfile
import urllib.error
import urllib.request
import zipfile

MAX_BYTES = 8 * 1024 * 1024


def redact(text, root):
    for value in (str(Path(root).resolve()), str(Path.home())):
        for spelling in (value, value.replace('\\', '/'), value.replace('\\', '\\\\')):
            text = re.sub(re.escape(spelling), '<LOCAL>', text, flags=re.I)
    text = re.sub(r'(?im)^.*(?:authorization\s*:|(?:token|password|secret|webhook)\s*[=:]).*$', '<REDACTED CREDENTIAL LINE>', text)
    text = re.sub(r'https://(?:\w+\.)?discord(?:app)?\.com/api/(?:v\d+/)?webhooks/[^\s"\x27]+', '<REDACTED WEBHOOK>', text)
    text = re.sub(r'(?i)[A-Z]:[\\/]Users[\\/][^\\/\s"\x27]+', '<USER>', text)
    return text


def atomic_json(path, data):
    pending = path.with_suffix('.pending')
    pending.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
    os.replace(pending, path)


def prepare(root, save_slot=None, description='', test=False):
    root = Path(root).resolve()
    session = json.loads((root/'play/last-session.json').read_text(encoding='utf-8'))
    identity = session['session']
    if not re.fullmatch('[a-f0-9]{32}', identity):
        raise ValueError('Invalid session identity')
    snapshot = root/'play/sessions'/identity
    identity = hashlib.sha256((identity+'|'+(save_slot or '')+'|'+description[:500]).encode('utf-8')).hexdigest()[:32]
    if snapshot.is_symlink() or snapshot.is_junction():
        raise ValueError('Linked session is not accepted')
    reports = root/'bug-reports'
    reports.mkdir(exist_ok=True)
    archive = reports/('Macross30-'+identity+'.zip')
    record = archive.with_suffix('.receipt.json')
    if archive.is_file() and record.is_file():
        result = json.loads(record.read_text(encoding='utf-8'))
        if hashlib.sha256(archive.read_bytes()).hexdigest() != result['sha256']:
            raise ValueError('Preserved report ZIP changed')
        return result
    metadata = {key: session[key] for key in ('game_version','launcher_version','language','exit_code')}
    metadata.update(description=redact(description[:500],root), test=bool(test))
    if not all(re.fullmatch(r'\d+\.\d+\.\d+', metadata[key] or '') for key in ('game_version','launcher_version')):
        raise ValueError('Verified game and launcher versions are required')
    names = {'stdout.log','stderr.log','process.json','exit.json','session.json','settings.ini','keymap.ini','padmap.ini'}
    names.update(p.name for p in snapshot.glob('diagnostics*.log'))
    inventory = []
    temporary = archive.with_suffix('.pending')
    try:
        with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_DEFLATED) as output:
            for name in sorted(names):
                path = snapshot/name
                if not path.is_file() or path.is_symlink():
                    continue
                with path.open('rb') as stream:
                    stream.seek(max(0,path.stat().st_size-192*1024))
                    data = redact(stream.read().decode('utf-8',errors='replace'),root).encode('utf-8')
                output.writestr('logs/'+name,data)
                inventory.append({'file':'logs/'+name,'bytes':len(data)})
            if save_slot:
                if save_slot in ('.','..') or Path(save_slot).name != save_slot or re.search(r'[\\/:]',save_slot):
                    raise ValueError('Invalid save slot')
                saves = root/'play/gamedata/dev_hdd0/home/00000001/savedata'
                selected = saves/save_slot
                for part in (selected,*selected.parents):
                    if part.is_symlink() or part.is_junction():
                        raise ValueError('Linked save is not accepted')
                    if part == root: break
                if not selected.is_dir(): raise ValueError('Selected save is missing')
                size = 0
                for path in selected.rglob('*'):
                    if path.is_symlink() or path.is_junction(): raise ValueError('Linked save is not accepted')
                    if not path.is_file(): continue
                    size += path.stat().st_size
                    if size > 4*1024*1024: raise ValueError('Selected save exceeds 4 MiB; logs can be sent without it')
                    name = 'selected-save/'+path.relative_to(selected).as_posix()
                    output.write(path,name)
                    inventory.append({'file':name,'bytes':path.stat().st_size})
            info = {'report_id':identity,**metadata,'system':{'os':platform.system(),'release':platform.release(),'architecture':platform.machine()},
                    'files':inventory,'save_included':bool(save_slot),'log_tail_limit_bytes':192*1024}
            output.writestr('report.json',json.dumps(info,ensure_ascii=False,indent=2))
        if temporary.stat().st_size > MAX_BYTES: raise ValueError('Report exceeds 8 MiB; local ZIP retained for manual support')
        os.replace(temporary,archive)
    except Exception:
        # Only the owned temporary is removed; the source session/save is never edited.
        temporary.unlink(missing_ok=True)
        raise
    result = {'report_id':identity,'archive':str(archive),'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
              'bytes':archive.stat().st_size,'metadata':metadata,'files':inventory,'status':'prepared','save_slot':save_slot}
    atomic_json(record,result)
    return result


def send(report, endpoint, cancelled=None, opener=None, allow_local_test=False):
    if opener is None:
        from network_tls import urlopen
        opener = urlopen
    from urllib.parse import urlparse
    url = urlparse(endpoint)
    if not (url.scheme == 'https' and url.netloc and not url.username and not url.query and not url.fragment):
        if not (allow_local_test and url.scheme == 'http' and url.hostname in ('127.0.0.1','localhost')):
            raise ValueError('An HTTPS report relay is required')
    path = Path(report['archive'])
    record = path.with_suffix('.receipt.json')
    if report.get('status') == 'sent': return report
    if cancelled and cancelled.is_set(): return report
    data = path.read_bytes()
    if len(data) > MAX_BYTES or hashlib.sha256(data).hexdigest() != report['sha256']:
        raise ValueError('Preserved report identity mismatch')
    target = endpoint.rstrip('/')+'/v1/reports/'+report['report_id']
    header = base64.b64encode(json.dumps(report['metadata'],ensure_ascii=True).encode('ascii')).decode('ascii')
    request = urllib.request.Request(target,data=data,method='PUT',headers={
        'Content-Type':'application/zip','X-Report-Sha256':report['sha256'],'X-Report-Metadata':header,
        'User-Agent':'Macross30Launcher/'+report['metadata']['launcher_version']})
    try:
        with opener(request,timeout=35) as response:
            result = json.loads(response.read(32*1024))
        if result.get('report_id') != report['report_id'] or result.get('sha256') != report['sha256']:
            raise ValueError('Relay receipt does not match this report')
        if result.get('status') == 'sent' and re.fullmatch(r'\d+',result.get('receipt_id') or ''):
            report.update(status='sent',receipt_id=result['receipt_id'],receipt_url=target)
        else:
            report.update(status='pending')
        atomic_json(record,report)
        return report
    except Exception:
        # A timeout may occur after delivery. Retrying the same report ID lets the
        # relay return its existing receipt without creating a duplicate message.
        report['status'] = 'retry'
        atomic_json(record,report)
        raise RuntimeError('Report was not confirmed. The local ZIP is preserved; retry the same report.') from None
