import os
from pathlib import Path
import shutil
import subprocess
import time

run=Path(__file__).resolve().parent
project=run.parent.parent
reference=project/'frame-reference-655360'
for directory in ('save','profile/AppData/Local','profile/AppData/Roaming','temp','shots','missing'):
    (run/directory).mkdir(exist_ok=True,parents=True)
for name in ('pkp.exe','pkp_rt.dll','astcdec-avx2-shared.dll','astcdec-sse2-shared.dll','astcdec-sse4.1-shared.dll',
             'controls.ini','pipeline_cache.bin','pipeline_keys.bin','shader_order.txt'):
    source=reference/name
    target=run/name
    if not target.exists():
        shutil.copy2(source,target)
config=run/'reference.ini'
config.write_text('''[fs]
exefs=G:/pokopia_dump_v2-0-1/updates/655360/exefs
romfs=G:/pokopia_dump_v2-0-1/updates/655360/romfs
[boot]
headless_width=1920
headless_height=1080
''',encoding='ascii')
frames=[540,660,780,900,1000,*range(1100,3701,100),4000,4300,4600,5000,5500,6000]
env=dict(os.environ, USERPROFILE=str(run/'profile'), LOCALAPPDATA=str(run/'profile/AppData/Local'),
         APPDATA=str(run/'profile/AppData/Roaming'), TEMP=str(run/'temp'), TMP=str(run/'temp'),
         NXRT_FS_SAVE_DIR=str(run/'save'), NXRT_NVN_SHADER_CACHE=str(reference/'shader_cache/spirv'),
         NXRT_NVN_DUMP_MISSING=str(run/'missing'), NXRT_NVN_TRACE_FRAME=','.join(map(str,frames)),
         NXRT_NVN_TRACE_SEC='42', NXRT_NVN_TRACE_SNAP='0', NXRT_NVN_TRACE_SNAP_SAMPLED='0',
         NXRT_NVN_TRACE_UBO_BYTES='64', NXRT_NVN_SHOT_DIR=str(run/'shots'),
         NXRT_NVN_SHOT_FRAMES=','.join(str(i+1) for i in frames))
start=time.monotonic()
with (run/'stdout.log').open('wb') as stdout, (run/'stderr.log').open('wb') as stderr:
    with subprocess.Popen([str(run/'pkp.exe'),'--ini',str(config),'--headless','--max-seconds','58','--log','title-reference.log'],
                          cwd=run,env=env,creationflags=subprocess.CREATE_NO_WINDOW,stdout=stdout,stderr=stderr) as proc:
        print('pid',proc.pid,flush=True)
        try:
            rc=proc.wait(timeout=70)
        except subprocess.TimeoutExpired:
            proc.terminate()
            rc=proc.wait()
        print('exit',rc,'seconds',round(time.monotonic()-start,2),flush=True)
