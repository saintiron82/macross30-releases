"""Read-only color/component evidence from actual combat P6 PPM/BMP frames.

Never controls input. Candidate colors do not establish target identity, lock,
HP, range or camera-axis polarity. Aim reference follows this title's observed
reticle at (640,288) in a1280x720 capture, not the image midpoint.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from run_battle_preview import read_rgb


def read_frame(path):
    raw=Path(path).read_bytes()
    if not raw.startswith(b'\x89PNG\r\n\x1a\n'):return read_rgb(path)
    pos=8;compressed=bytearray()
    while pos<len(raw):
        n=struct.unpack_from('>I',raw,pos)[0];tag=raw[pos+4:pos+8];v=raw[pos+8:pos+8+n]
        if tag==b'IHDR':w,h,depth,color,_,_,interlace=struct.unpack('>IIBBBBB',v)
        if tag==b'IDAT':compressed.extend(v)
        pos+=n+12
    if depth!=8 or color not in (2,6) or interlace:raise ValueError('PNG requires noninterlaced8bitRGB/RGBA')
    bpp=3 if color==2 else 4;pitch=w*bpp;src=zlib.decompress(compressed);prev=bytearray(pitch);out=bytearray();pos=0
    def paeth(a,b,c):
        p=a+b-c;ds=[abs(p-a),abs(p-b),abs(p-c)];return (a,b,c)[ds.index(min(ds))]
    for _ in range(h):
        kind=src[pos];row=bytearray(src[pos+1:pos+1+pitch]);pos+=pitch+1
        if kind>4:raise ValueError('InvalidPNGfilter')
        for i in range(pitch):
            left=row[i-bpp] if i>=bpp else 0;up=prev[i];ul=prev[i-bpp] if i>=bpp else 0
            row[i]=(row[i]+(0,left,up,(left+up)//2,paeth(left,up,ul))[kind])&255
        out.extend(row if bpp==3 else bytes(x for i,x in enumerate(row) if i%4!=3));prev=row
    return w,h,bytes(out)


def components(points, radius=2):
    # Coarse cells join tiny glyph gaps; report original-pixel bounds/counts.
    cells = {}
    for x,y in points:
        cells.setdefault((x//radius,y//radius),[]).append((x,y))
    result=[]
    while cells:
        key=next(iter(cells));todo=[key];pixels=[]
        while todo:
            x,y=todo.pop();v=cells.pop((x,y),None)
            if v is None:continue
            pixels.extend(v)
            todo.extend((x+dx,y+dy) for dx in (-1,0,1) for dy in (-1,0,1)
                        if (x+dx,y+dy) in cells)
        xs=[p[0] for p in pixels];ys=[p[1] for p in pixels]
        result.append({'box':[min(xs),min(ys),max(xs)+1,max(ys)+1],
                       'pixels':len(pixels),'centroid':[sum(xs)/len(xs),sum(ys)/len(ys)]})
    return sorted(result,key=lambda x:x['pixels'],reverse=True)


def inspect(path):
    w,h,data=read_frame(path);sx=w/1280;sy=h/720
    aim=[640*sx,288*sy]
    # Excludes fixed top-left guide, radar, lower-left gauges, weapon HUD,
    # subtitles/rewards and top-center ENEMY APPEARED banner.
    roi=[int(350*sx),int(180*sy),int(930*sx),int(480*sy)]
    yellow=[];red=[]
    for y in range(roi[1],roi[3]):
        for x in range(roi[0],roi[2]):
            i=(y*w+x)*3;r,g,b=data[i:i+3]
            if r>=140 and g>=130 and b<100 and min(r,g)>b*1.8:yellow.append((x,y))
            if r>=150 and g<95 and b<100 and r>max(g,b)*1.8:red.append((x,y))
    yc=[c for c in components(yellow,max(2,round(3*sx))) if c['pixels']>=6]
    rc=[c for c in components(red,max(2,round(3*sx))) if c['pixels']>=6]
    # A banner is often separate connected letters, so width of a SINGLE
    # component is insufficient. Reject dense, wide red overlay unions too.
    red_span=(max(x for x,y in red)-min(x for x,y in red)) if red else 0
    warning=(sum(c['pixels'] for c in rc if c['box'][2]-c['box'][0]>180*sx)>300*sx*sy or
             len(red)>3000*sx*sy and red_span>300*sx)
    candidates=[]
    for c in yc:
        x0,y0,x1,y1=c['box'];bw=x1-x0;bh=y1-y0
        # World arrow/distance/name glyph groups only a candidate; dense
        # central damage text remains possible and is explicitly ambiguous.
        if bw<8*sx or bw>180*sx or bh>75*sy:continue
        cx,cy=c['centroid'];dx=cx-aim[0];dy=cy-aim[1]
        candidates.append(dict(c,aim_offset_pixels=[dx,dy],
            aim_offset_fraction=[dx/w,dy/h],screen_direction=[
                'right' if dx>12*sx else 'left' if dx<-12*sx else 'center',
                'below' if dy>12*sy else 'above' if dy<-12*sy else 'level']))
    # Bracket-like red evidence must exist on BOTH sides of the actual aim
    # reference. Wide boundary/banner components are excluded.
    central=[c for c in rc if c['box'][2]-c['box'][0]<100*sx and
             abs(c['centroid'][0]-aim[0])<85*sx and abs(c['centroid'][1]-aim[1])<65*sy]
    bilateral=any(c['centroid'][0]<aim[0]-10*sx for c in central) and any(c['centroid'][0]>aim[0]+10*sx for c in central)
    # Dim yellow name leader can extend OUTSIDE the clamped arrow ROI. Search
    # wider, suppressing known radar and weapon/reward HUD areas. Its endpoint
    # is image evidence only, never a guaranteed target/world-coordinate.
    dim=[]
    for y in range(int(180*sy),int(470*sy)):
        for x in range(int(350*sx),int(1270*sx)):
            if ((x/sx-1050)**2+(y/sy-177)**2<130**2 or
                    1160*sx<=x<=1190*sx and y<275*sy):continue
            i=(y*w+x)*3;r,g,b=data[i:i+3]
            if 35<=r<=180 and 35<=g<=190 and b<55 and min(r,g)>b*1.9 and max(r,g)<min(r,g)*1.8:
                dim.append((x,y))
    leaders=[]
    for c in components(dim,max(2,round(3*sx))):
        x0,y0,x1,y1=c['box'];bw=x1-x0;bh=y1-y0
        if c['pixels']>=25 and 20*sx<=bw<=210*sx and 30*sy<=bh<=180*sy:
            leaders.append(dict(c,bounds_only=True,
                note='Dim yellow elongated component; may be leader/text/scene. Visual review required. No inferred HP, lock or target anchor.'))
    return {'file':str(path),'sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            'size':[w,h],'aim_reference':aim,'world_search_roi':roi,
            'yellow_candidates':candidates,'red_components':rc,
            'red_pixel_count':len(red),
            'dim_yellow_leader_candidates':leaders,
            'central_red_bilateral_evidence':bilateral and not warning,
            'large_red_overlay_contamination':warning,
            'semantics':'Color/geometry evidence only; no OCR, target identity, HP, actual lock or input-axis polarity inferred. Multiple candidates require visual review. No input emitted.'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('frames',nargs='+',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=[inspect(f) for f in a.frames]
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(r,indent=2),encoding='utf8')
    print(json.dumps([{'file':x['file'],'candidates':len(x['yellow_candidates']),
        'central_red':x['central_red_bilateral_evidence'],'overlay':x['large_red_overlay_contamination']} for x in r],indent=2))


if __name__=='__main__':main()
