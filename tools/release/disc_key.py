"""Fetch the supported Japanese disc-key entry from a pinned public catalog."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.request

from decrypt_iso import key_bytes,ROOT

CATALOG_REVISION='66256a42ce2bd95056f32dba78963608711c0852'
CATALOG_URL='https://raw.githubusercontent.com/aldostools/Resources/'+CATALOG_REVISION+'/REDUMP_DKEY.7z'
CATALOG_SHA256='859343538cfd864621abc31029c12c42c2ea7f59dd8d7fbebe8b509e58c34903'
SEVENZIP_SHA256='256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282'
KEY_NAME='Macross 30 - Ginga o Tsunagu Utagoe (Japan).dkey'


def resolve(directory,sevenzip,opener=urllib.request.urlopen):
    directory=Path(directory).resolve();directory.mkdir(parents=True,exist_ok=True)
    target=directory/KEY_NAME
    if target.is_file():
        key_bytes(target)
        return dict(path=str(target),reused=True,key_printed=False)
    sevenzip=Path(sevenzip).resolve(strict=True)
    if hashlib.sha256(sevenzip.read_bytes()).hexdigest()!=SEVENZIP_SHA256:
        raise ValueError('Unexpected archive utility hash')
    with tempfile.TemporaryDirectory(prefix='disc-key-',dir=directory) as td:
        work=Path(td)
        with opener(CATALOG_URL,timeout=30) as response:catalog=response.read((1<<20)+1)
        if len(catalog)>1<<20 or hashlib.sha256(catalog).hexdigest()!=CATALOG_SHA256:
            raise ValueError('Public key catalog hash mismatch')
        (work/'catalog.7z').write_bytes(catalog)
        subprocess.run([str(sevenzip),'e','catalog.7z','-oextracted','-y','REDKEY\\'+KEY_NAME],cwd=work,
                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True,
                       creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),timeout=30)
        extracted=work/'extracted'/KEY_NAME
        key_bytes(extracted)
        # Never overwrite an existing user key or publish its value in command output.
        with target.open('xb') as stream:stream.write(extracted.read_bytes())
    result=dict(path=str(target),reused=False,key_printed=False,catalog_url=CATALOG_URL,catalog_sha256=CATALOG_SHA256)
    (directory/'key-source.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path,default=ROOT/'.tools/iso-private')
    parser.add_argument('--sevenzip',type=Path,required=True)
    args=parser.parse_args()
    print(json.dumps(resolve(args.output_dir,args.sevenzip),indent=2))
