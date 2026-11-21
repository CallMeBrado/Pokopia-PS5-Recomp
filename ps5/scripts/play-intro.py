#!/usr/bin/env python3
"""Drive a fresh launch through the opening with the virtual pad: language
select (English, Yes), the autosave notice, the wake-up dialogue, character
select and customisation. The save does not persist between launches, so each
test run starts at language select.

    python ps5/scripts/play-intro.py            # wait for the title, then play
    python ps5/scripts/play-intro.py --stage character   # stop before character select

Presses are held 700 ms: menus run at a few frames per second and short taps
fall between frames."""
import argparse
import importlib.util
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ps5ctl", os.path.join(HERE, "ps5-ctl.py"))
ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctl)

STAGES = [
    ("language", [("A", 4), ("UP", 3), ("A", 25), ("A", 25)]),
    ("intro", [("A", 6), ("A", 6), ("A", 15), ("A", 8), ("A", 5), ("A", 5), ("A", 20),
               ("A", 8), ("A", 8), ("A", 8), ("A", 8), ("A", 15)]),
    ("character", [("LEFT", 4), ("A", 8), ("A", 6), ("A", 6), ("A", 6), ("A", 8), ("PLUS", 6), ("A", 6)]),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", choices=[name for name, _ in STAGES], help="stop before this stage")
    parser.add_argument("--no-wait", action="store_true", help="do not wait for the title screen first")
    args = parser.parse_args()
    if not args.no_wait:
        subprocess.run([sys.executable, os.path.join(HERE, "watch-run.py"), "--tag", "intro",
                        "--gameplay-windows", "2", "--max-s", "300"], check=True)
    for name, presses in STAGES:
        if name == args.stage:
            break
        print("stage", name, flush=True)
        for button, wait in presses:
            if not ctl.pad([button], 700, (0, 0, 0, 0)):
                print("no acknowledgement for", button, flush=True)
            time.sleep(wait)


if __name__ == "__main__":
    main()
