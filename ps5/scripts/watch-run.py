"""Watch a PS5 run: fetch the boot log and diagnostics every 20 s until the
game has drawn gameplay frames for a while, stops, crashes, or time runs out.

    python ps5/scripts/watch-run.py [--tag r70] [--gameplay-windows 6] [--max-s 600]

Files land in ps5/logs/live-<tag>*.txt. Prints one progress line per poll.
"""
import argparse
import ftplib
import os
import re
import time

HOST = os.environ.get("PS5_HOST", "172.30.0.212")
FILES = {"log": "ps5-game-boot-r65.txt", "prof": "ps5-prof.txt", "timeline": "ps5-timeline.txt",
         "fsp": "ps5-fsp.txt"}


def fetch(tag, logs):
    out = {}
    ftp = ftplib.FTP()
    ftp.connect(HOST, 2121, timeout=60)
    ftp.login()
    ftp.cwd("/data/pokopia")
    for key, name in FILES.items():
        chunks = []
        try:
            ftp.retrbinary(f"RETR {name}", chunks.append)
        except ftplib.all_errors:
            continue
        data = b"".join(chunks)
        suffix = "" if key == "log" else f"-{key}"
        with open(os.path.join(logs, f"live-{tag}{suffix}.txt"), "wb") as fh:
            fh.write(data)
        out[key] = data.decode("utf-8", "replace")
    ftp.quit()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="run")
    ap.add_argument("--gameplay-windows", type=int, default=6)
    ap.add_argument("--max-s", type=int, default=600)
    ap.add_argument("--until-t", type=float, default=0, help="stop once the guest clock passes this (s)")
    args = ap.parse_args()
    logs = os.path.join(os.path.dirname(__file__), "..", "logs")
    start = time.time()
    while True:
        try:
            got = fetch(args.tag, logs)
        except (OSError, EOFError, ftplib.all_errors) as error:
            print(f"{time.time() - start:5.0f}s fetch failed: {error}", flush=True)
            got = {}
        log = got.get("log", "")
        draws = re.findall(r"general draws ok=(\d+)", log)
        gameplay = sum(1 for d in draws if int(d) > 0)
        presents = re.findall(r"presents=(\d+)", log)
        stamp = re.findall(r"Load stats t=([0-9.]+)s", log)
        print(f"{time.time() - start:5.0f}s log={len(log)} guest_t={stamp[-1] if stamp else '-'} "
              f"presents={presents[-1] if presents else '-'} gameplay_windows={gameplay}", flush=True)
        if re.search(r"^(STOP|Fatal signal)", log, re.M):
            print("stopped", flush=True)
            return
        if args.until_t and stamp and float(stamp[-1]) >= args.until_t:
            print("time reached", flush=True)
            return
        if gameplay >= args.gameplay_windows:
            print("gameplay reached", flush=True)
            return
        if time.time() - start > args.max_s:
            print("timed out", flush=True)
            return
        time.sleep(20)


if __name__ == "__main__":
    main()
