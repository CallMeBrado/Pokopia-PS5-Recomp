"""Fetch the PS5 boot log, save it under ps5/logs/, and print a summary.

    python ps5/scripts/fetch-log.py [--name ps5-game-boot-r65.txt] [--tail 40] [--save-as run-01]
"""
import argparse
import datetime
import ftplib
import os
import re

HOST = os.environ.get("PS5_HOST", "172.30.0.212")
KEY = re.compile(r"^(PASS|FAIL|STOP|Reached|Pokopia backend|Vulkan adapter|SetHeapSize|NVN acquired|Graphics skipped)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="ps5-game-boot-r65.txt")
    ap.add_argument("--tail", type=int, default=40)
    ap.add_argument("--save-as")
    args = ap.parse_args()
    ftp = ftplib.FTP()
    ftp.connect(HOST, 2121, timeout=120)
    ftp.login()
    ftp.cwd("/data/pokopia")
    chunks = []
    ftp.retrbinary(f"RETR {args.name}", chunks.append)
    ftp.quit()
    data = b"".join(chunks)
    logs = os.path.join(os.path.dirname(__file__), "..", "logs")
    os.makedirs(logs, exist_ok=True)
    stamp = args.save_as or datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(logs, f"{stamp}-{args.name}")
    with open(path, "wb") as fh:
        fh.write(data)
    lines = data.decode("utf-8", "replace").splitlines()
    print(f"{len(lines)} lines, {len(data)} bytes -> {os.path.normpath(path)}")
    seen = set()
    print("--- milestones")
    for line in lines:
        m = KEY.match(line)
        if m and (m.group(1) not in ("NVN acquired", "Graphics skipped") or m.group(1) not in seen):
            seen.add(m.group(1))
            print(line[:300])
    print(f"--- last {args.tail} lines")
    for line in lines[-args.tail:]:
        print(line[:400])


if __name__ == "__main__":
    main()
