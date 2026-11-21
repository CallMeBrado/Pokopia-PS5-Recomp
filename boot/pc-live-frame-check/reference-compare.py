"""Compare read-only guest state with the existing 2.0.1 Windows port."""

import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import time


scratch = Path(__file__).resolve().parent
reference = scratch.parent.parent / "frame-reference-655360"
snapshot = runpy.run_path(str(scratch / "read-boot-state.py"))["snapshot"]


def prepare_reference(with_save, suffix):
    run = scratch / (("reference-existing-save" if with_save else "reference-empty-save") + suffix)
    run.mkdir(exist_ok=False)
    for name in ("shots", "temp", "profile/Local", "profile/Roaming"):
        (run / name).mkdir(parents=True)
    if with_save:
        shutil.copytree(reference / "save", run / "save")
    else:
        (run / "save").mkdir()
    (run / "pkp.ini").write_text("""[fs]
exefs = G:/pokopia_dump_v2-0-1/updates/655360/exefs
romfs = G:/pokopia_dump_v2-0-1/updates/655360/romfs
[boot]
headless_width = 1920
headless_height = 1080
""", encoding="ascii")
    env = dict(os.environ, USERPROFILE=str(run / "profile"), LOCALAPPDATA=str(run / "profile/Local"),
               APPDATA=str(run / "profile/Roaming"), TEMP=str(run / "temp"), TMP=str(run / "temp"),
               NXRT_FS_SAVE_DIR=str(run / "save"), NXRT_NVN_SHADER_CACHE=str(reference / "shader_cache/spirv"),
               NXRT_NVN_TRACE_FRAME="520,540,560,620", NXRT_NVN_SHOT_DIR=str(run / "shots"),
               NXRT_NVN_SHOT_FRAMES="521,541,561,621")
    return run, env


def compare(with_save, suffix):
    run, env = prepare_reference(with_save, suffix)
    with (run / "stdout.txt").open("wb") as stdout, (run / "stderr.txt").open("wb") as stderr, \
         (run / "state.txt").open("w", encoding="ascii") as state_log:
        with subprocess.Popen([str(reference / "pkp.exe"), "--headless", "--max-seconds", "16", "--log", "reference.log"],
                              cwd=run, env=env, creationflags=subprocess.CREATE_NO_WINDOW,
                              stdout=stdout, stderr=stderr) as proc:
            start = time.monotonic()
            previous = None
            last_report = 0.0
            while proc.poll() is None and time.monotonic() - start < 30:
                elapsed = time.monotonic() - start
                try:
                    state = snapshot(proc.pid)
                except OSError:
                    time.sleep(0.025)
                    continue
                if state != previous or elapsed - last_report >= 1.0:
                    line = f"{elapsed:.3f}s {state}"
                    state_log.write(line + "\n")
                    state_log.flush()
                    previous, last_report = state, elapsed
                time.sleep(0.025)
            if proc.poll() is None:
                proc.terminate()
    print(run)


if __name__ == "__main__":
    compare("--existing-save" in sys.argv[1:], "-scene" if "--scene" in sys.argv[1:] else "")
