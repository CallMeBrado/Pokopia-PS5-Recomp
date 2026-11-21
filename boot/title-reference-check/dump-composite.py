import os
from pathlib import Path
import subprocess

run = Path(__file__).resolve().parent
for name in ('dump-save','composite-missing'):
    (run / name).mkdir(exist_ok=True)
env = dict(os.environ, USERPROFILE=str(run/'profile'), LOCALAPPDATA=str(run/'profile/AppData/Local'),
           APPDATA=str(run/'profile/AppData/Roaming'), TEMP=str(run/'temp'), TMP=str(run/'temp'),
           NXRT_FS_SAVE_DIR=str(run/'dump-save'), NXRT_NVN_SHADER_CACHE=str(run/'selective-cache'),
           NXRT_NVN_DUMP_MISSING=str(run/'composite-missing'), NXRT_NVN_TRACE_SEC='19',
           NXRT_NVN_TRACE_SNAP='0', NXRT_NVN_TRACE_SNAP_SAMPLED='0', NXRT_NVN_TRACE_UBO_BYTES='512')
with (run/'dump-stdout.log').open('wb') as stdout, (run/'dump-stderr.log').open('wb') as stderr:
    with subprocess.Popen([str(run/'pkp.exe'),'--ini',str(run/'reference.ini'),'--headless','--max-seconds','29',
                           '--log','composite-dump.log'], cwd=run, env=env, creationflags=subprocess.CREATE_NO_WINDOW,
                          stdout=stdout,stderr=stderr) as proc:
        print('pid',proc.pid,flush=True)
        rc = proc.wait(timeout=42)
        print('exit',rc,flush=True)
