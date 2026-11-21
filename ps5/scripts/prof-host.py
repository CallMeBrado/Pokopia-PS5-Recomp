#!/usr/bin/env python3
"""Per-function host profile from /data/pokopia/ps5-prof.txt.

Fetches the profile, takes the last report's "H tid pc count" lines, maps the
pcs to functions in the linked ELF (WSL PS5Build, llvm-symbolizer) and prints
the top functions per thread.

    python ps5/scripts/prof-host.py [--thread t24] [--top 40]"""
import argparse
import bisect
import collections
import ftplib
import io
import os
import re
import subprocess

HOST = os.environ.get("PS5_HOST", "172.30.0.212")
ELF = "/root/ps5/build/pokopia/link/pokopia.elf"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--thread")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()
    client = ftplib.FTP()
    client.connect(HOST, 2121, timeout=30)
    client.login()
    data = io.BytesIO()
    client.retrbinary("RETR /data/pokopia/ps5-prof.txt", data.write)
    client.quit()
    lines = data.getvalue().decode(errors="replace").splitlines()
    heads = [i for i, line in enumerate(lines) if line.startswith("--- ")]
    if not heads:
        raise SystemExit("no report yet")
    head = heads[-1]
    anchor = int(re.search(r"fatal_signal at (?:0x)?([0-9a-f]+)", lines[head])[1], 16)
    samples = []
    for line in lines[head + 1:]:
        if line.startswith("T "):
            print(line)
        match = re.match(r"H([SUV]?) (t\d+) (0x[0-9a-f]+) (\d+)", line)
        if match and (not args.thread or match[2] == args.thread):
            samples.append((match[2], int(match[3], 16), int(match[4]), match[1]))
    if not samples:
        raise SystemExit("no host samples in the last report")
    table = subprocess.run(["wsl", "-d", "PS5Build", "--", "bash", "-lc",
                            f"source /mnt/e/Pokopia-PS4/ps5/build/env.sh; $SDK/bin/llvm-nm -n --defined-only {ELF}"],
                           capture_output=True, text=True).stdout.splitlines()
    symbols = []
    for row in table:
        parts = row.split()
        if len(parts) >= 3 and parts[1] in "tTwW":
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    starts = [address for address, _ in symbols]
    anchor_link = next(address for address, name in symbols if name == "fatal_signal")
    slide = anchor - anchor_link
    names = {}
    for sample in samples:
        pc = sample[1]
        index = bisect.bisect_right(starts, pc - slide) - 1
        names[pc] = symbols[index][1] if index >= 0 and pc < 0x800000000 else "?"
        if index >= len(symbols) - 1 or (index >= 0 and pc - slide - symbols[index][0] > 0x2000 and
                                          symbols[index][1] == symbols[-1][1]):
            names[pc] = "(not code: stack data)"
    per = collections.defaultdict(collections.Counter)
    for tid, pc, count, syscall in samples:
        name = names.get(pc, "?")
        if name in ("??", "?"):
            name = f"?{pc:#x}" if pc < 0x800000000 else "system-lib"
        if syscall == "S":
            name = "[kernel call from] " + name
        elif syscall == "U":
            name = "    [kernel call, 2 up] " + name
        elif syscall == "V":
            name = "        [kernel call, 3 up] " + name
        per[tid][name] += count
    for tid, counter in sorted(per.items(), key=lambda item: -sum(item[1].values())):
        total = sum(counter.values())
        print(f"== {tid} host samples={total}")
        for name, count in counter.most_common(args.top):
            print(f"  {count:6d} {100.0 * count / total:5.1f}%  {name}")


if __name__ == "__main__":
    main()
