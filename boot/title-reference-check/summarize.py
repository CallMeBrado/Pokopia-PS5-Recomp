from pathlib import Path
from collections import defaultdict, Counter
import re
from PIL import Image, ImageChops

root=Path(__file__).resolve().parent
frames=defaultdict(list)
frame=None
frame_clock={}
for number,line in enumerate((root/'title-reference.log').read_text(encoding='utf-8',errors='replace').splitlines(),1):
    match=re.search(r'\[\s*([0-9.]+).*nvn.trace: ---- frame (\d+) ----',line)
    if match:
        frame=int(match[2]); frame_clock.setdefault(frame,float(match[1]))
    elif frame is not None and (match:=re.search(r'nvn.trace: draw #(\d+) vs (\S+) fs (\S+) gs (\S+) mode (\d+) idx (\d+) \|\s+rt (\S+)',line)):
        frames[frame].append((number,*match.groups()))
for path in sorted((root/'shots').glob('*.png')):
    if not path.stem.isdigit():
        continue
    frame=int(path.stem)-1
    with Image.open(path) as picture:
        red,green,blue=picture.convert('RGB').split()
        brightness=ImageChops.lighter(red,ImageChops.lighter(green,blue))
        p16=brightness.point(lambda v:255 if v>=16 else 0)
        amount=p16.histogram()[255]
    window=Counter((vs,fs,gs) for _,_,vs,fs,gs,_,_,target in frames[frame] if re.match(r't[123]:RGBA8_SRGB',target))
    extras=Counter((vs,fs,gs) for _,_,vs,fs,gs,_,_,_ in frames[frame])
    print(f'frame={frame} time={frame_clock.get(frame,0):.3f} nonblack={amount} bbox={p16.getbbox()} draws={len(frames[frame])} unique={len(extras)} window={dict(window)}')
