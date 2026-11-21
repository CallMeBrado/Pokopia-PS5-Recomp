#!/usr/bin/env python3
"""Poll the boot log while the game runs in the world and exit as soon as it
stops (STOP / Fatal / boundary), or after --max-s seconds. Prints new problem
lines and a frame-rate line from each stats window as they appear. Takes one
screenshot after --shot-s seconds.

    python ps5/scripts/watch-world.py --max-s 180 --shot-s 30"""
import argparse
import ftplib
import io
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.environ.get("PS5_HOST", "172.30.0.212")
LOG = "/data/pokopia/ps5-game-boot-r65.txt"
PROBLEMS = ("STOP", "Fatal", "fsp save", "fsp: ", "NVN boundary", "NVN invalid", "bind compute", "Live FPS",
            "SDK requested", "appletOE: unsupported")


def fetch():
    client = ftplib.FTP()
    client.connect(HOST, 2121, timeout=30)
    client.login()
    data = io.BytesIO()
    client.retrbinary(f"RETR {LOG}", data.write)
    client.quit()
    return data.getvalue().decode(errors="replace").splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-s", type=float, default=180)
    parser.add_argument("--shot-s", type=float, default=30)
    parser.add_argument("--every", type=float, default=10)
    parser.add_argument("--save", default=os.path.join(HERE, "..", "logs", "boot-watch.txt"))
    args = parser.parse_args()
    start = time.time()
    seen = 0
    shot = False
    while True:
        try:
            lines = fetch()
        except OSError as error:
            print("log fetch failed:", error, flush=True)
            lines = None
        if lines is not None:
            if len(lines) < seen:
                seen = 0
            stopped = False
            for line in lines[seen:]:
                if line.startswith(PROBLEMS):
                    print(line[:220], flush=True)
                    stopped |= line.startswith(("STOP", "Fatal"))
                elif line.startswith(("General draw timing", "General draw upload", "Draw batch flush", "Graphics lock", "Replay worker", "General draw direct", "Guest heap import")):
                    print(line[:200], flush=True)
                elif line.startswith("Load stats"):
                    match = re.search(r"t=([\d.]+)s window_ms=(\d+) presents=(\d+)", line)
                    if match:
                        t, window, presents = float(match[1]), float(match[2]), int(match[3])
                        print(f"t={t:.0f}s fps={presents * 1000 / window:.1f}", flush=True)
            seen = len(lines)
            with open(args.save, "w", encoding="utf-8") as out:
                out.write("\n".join(lines))
            if stopped:
                return 1
        elapsed = time.time() - start
        if not shot and elapsed >= args.shot_s:
            shot = True
            subprocess.run([sys.executable, os.path.join(HERE, "ps5-ctl.py"), "shot"], check=False)
        if elapsed >= args.max_s:
            print("still running after", int(elapsed), "s", flush=True)
            return 0
        time.sleep(args.every)


if __name__ == "__main__":
    sys.exit(main())
