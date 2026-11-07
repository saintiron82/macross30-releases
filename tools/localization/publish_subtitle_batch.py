"""Validate complete translated ASR batches and publish guarded draft sidecars."""
import argparse
import hashlib
import json
from pathlib import Path
from transcribe_movie_audio import stamp
from caption_text import clean_caption_text


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def load(path):return json.loads(path.read_text(encoding='utf-8'))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--batch',type=Path,action='append',required=True)
    p.add_argument('--output',type=Path,default=Path('localization/subtitles/runtime'))
    n=p.parse_args()
    inventory=load(Path('localization/subtitles/audio/batch-extraction-manifest.json'))
    audio={Path(r['source']).stem:r for r in inventory['results']}
    tracks={}
    if (n.output/'tracks.tsv').exists():
        lines=(n.output/'tracks.tsv').read_text(encoding='utf-8').splitlines()
        if not lines or lines[0]!='MACROSS_SUBTITLES_V1':raise ValueError('Bad existing registry')
        for line in lines[1:]:
            fields=line.split('\t')
            if len(fields)!=7:raise ValueError('Bad existing registry row')
            tracks[fields[0]]=fields
    planned=[]
    seen=set()
    for batch in n.batch:
        doc=load(batch)
        original_batch=batch.with_name(batch.name.replace('.ko.json','.json'))
        if original_batch==batch or not original_batch.is_file():raise ValueError('Original translation work batch required')
        expected=load(original_batch)
        if [r['movie'] for r in doc['clips']]!=[r['movie'] for r in expected['clips']]:
            raise ValueError('Translated batch is incomplete or reordered')
        for clip in doc['clips']:
            movie=clip['movie']
            if movie in seen:raise ValueError('Duplicate movie in publication')
            seen.add(movie)
            if not movie.isalnum() and '_' not in movie:raise ValueError('Bad movie name')
            if any(c not in '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_' for c in movie):raise ValueError('Bad movie name')
            source=Path(clip['source_transcript'])
            if sha(source)!=clip['source_sha256']:raise ValueError('ASR source changed')
            raw=load(source)['segments'];cues=clip['cues']
            if len(raw)!=len(cues):raise ValueError('Missing/extra translated cue')
            previous=0
            for r,c in zip(raw,cues):
                if any(c[k]!=r[k] for k in ('id','start','end','source_ja')):raise ValueError('ASR identity/timing changed')
                text=c['target_ko']
                if not isinstance(text,str) or not text.strip() or '\0' in text or '\n\n' in text or len(text.encode())>4096:
                    raise ValueError('Invalid Korean cue')
                if not previous<=c['start']<c['end']<=audio[movie]['seconds']+.1:raise ValueError('Invalid cue bounds')
                previous=c['end']
            visible=[(c,clean_caption_text(c['target_ko'])) for c in cues]
            visible=[(c,text) for c,text in visible if text]
            content='\n\n'.join(f"{i}\n{stamp(c['start'])} --> {stamp(c['end'])}\n{text}" for i,(c,text) in enumerate(visible,1))+'\n'
            name=movie+'.clean.ko.srt';dest=n.output/name
            digest=hashlib.sha256(content.encode()).hexdigest()
            if dest.exists() and dest.read_text(encoding='utf-8')!=content:raise ValueError('Preserve differing existing subtitle '+name)
            planned.append((dest,content,dict(movie=movie,cues=len(visible),batch=str(batch),source_sha256=clip['source_sha256'],human_listening_verified=False)))
            a=audio[movie]
            tracks[movie+'.pam']=[movie+'.pam',name,str(a['first_pts90k']),'1',a['source_SHA256'],digest,'draft']
    n.output.mkdir(parents=True,exist_ok=True)
    for dest,content,meta in planned:
        dest.write_bytes(content.encode('utf-8'))
        if sha(dest)!=hashlib.sha256(content.encode('utf-8')).hexdigest():raise ValueError('Written subtitle hash mismatch')
        dest.with_suffix('.review.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    registry='MACROSS_SUBTITLES_V1\n'+'\n'.join('\t'.join(v) for k,v in sorted(tracks.items()))+'\n'
    temp=n.output/'tracks.next.tsv';temp.write_bytes(registry.encode('utf-8'));temp.replace(n.output/'tracks.tsv')
    report=dict(status='published_draft_sidecars',new_clips=len(planned),total_tracks=len(tracks),
                cues=sum(m['cues'] for d,c,m in planned),human_listening_verified=False,game_process_modified=False)
    (n.output/'batch-publication.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))


if __name__=='__main__':main()
