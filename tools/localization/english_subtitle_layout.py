"""Project long English cues into bounded pages without losing words or outer timing."""
import textwrap

def pages(text,width=38,max_lines=3):
    flat=' '.join(text.split())
    lines=textwrap.wrap(flat,width,break_long_words=False,break_on_hyphens=False)
    if not lines or any(len(line)>width for line in lines):raise ValueError('Empty subtitle or over-wide word')
    result=['\n'.join(lines[i:i+max_lines]) for i in range(0,len(lines),max_lines)]
    assert ' '.join(' '.join(result).split())==flat
    return result

def project(start,end,parts):
    a=round(start*1000);b=round(end*1000)
    if a<0 or b-a<len(parts):raise ValueError('Insufficient cue interval for nonempty pages')
    weights=[len(''.join(p.split())) for p in parts];total=sum(weights);cursor=a;done=0;result=[]
    for i,(part,weight) in enumerate(zip(parts,weights)):
        done+=weight
        stop=b if i+1==len(parts) else max(cursor+1,min(b-(len(parts)-i-1),round(a+(b-a)*done/total)))
        result.append((cursor/1000,stop/1000,part));cursor=stop
    assert result[0][0]==a/1000 and result[-1][1]==b/1000
    assert all(x[0]<x[1] for x in result) and all(x[1]==y[0] for x,y in zip(result,result[1:]))
    return result
