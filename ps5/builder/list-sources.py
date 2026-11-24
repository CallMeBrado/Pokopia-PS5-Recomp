#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write source-files.txt: every file of this repository the builder's runtime build and generators read.

Runs the runtime compile's preprocessor (`zig clang -M`, the flags of winbuild.py) over every runtime
source and keeps the repository files among their dependencies, minus what the builder generates from
the dump; then adds the build and generator scripts. make-release.py copies exactly these files into
the builder's source/ folder.

    python list-sources.py --config <winbuild.json>
"""
import argparse
import concurrent.futures
import importlib.util
import os
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

# Read or run by the builder besides the compiled sources and their headers.
SCRIPTS = [
    "ps5/build/winbuild.py", "ps5/build/compile-runtime.sh", "ps5/build/patch-lift.py",
    "ps5/build/compile-game.sh",
    "ps5/build/prepare-packed-floats.py", "ps5/build/pack-shaders.py", "ps5/title/param.json",
    "cpu-probe/generate.py", "loader-probe/prepare.py", "sdk-bringup/worker-site.py",
    "sdk-bringup/graphics-sites.py", "gfx-bridge/build-romfs-index.py",
]
# Made from the dump by the builder: never shipped.
GENERATED = ("cpu-probe/generated/", "loader-probe/generated/", "gfx-bridge/generated/", "gfx-bridge/sdk-graphics.inc",
             "sdk-bringup/graphics.sites")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("winbuild", REPO / "ps5/build/winbuild.py")
    winbuild = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(winbuild)
    b = winbuild.Build(a.config, os.cpu_count() or 4)
    c_sources, cpp_sources = b.runtime_sources()
    arch, defs, incs = b.ps5_flags()
    pinned = REPO / "upstream/switch2-recomp-0.2.0"
    cpp_incs = ["-I", str(REPO / "gfx-bridge/nvn-layout-include"), "-I", str(pinned / "runtime/include"),
                "-I", str(pinned / "runtime/src/gfx/nvn"), "-I", str(pinned / "runtime/src/gfx/nvn/third_party"),
                "-I", str(pinned / "tools/tiling/include")]
    jobs = [(s, ["-std=gnu11"] + arch + defs + incs + ["-I", str(REPO / "ps5/compat/vkloader")]) for s in c_sources]
    jobs += [(s, ["-std=c++20", "-fno-exceptions", "-fno-rtti"] + arch + defs + incs + cpp_incs) for s in cpp_sources]

    def deps(job):
        source, flags = job
        r = subprocess.run(b.clang + flags + ["-M", str(source)], capture_output=True, text=True, errors="replace")
        if r.returncode:
            raise SystemExit(f"{source}:\n{r.stderr[-2000:]}")
        text = r.stdout.replace("\\\n", " ")
        return [t for t in text.split(":", 1)[1].split() if t]

    found = set()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for paths in pool.map(deps, jobs):
            for p in paths:
                p = Path(p.replace("\\ ", " ")).resolve()
                try:
                    rel = p.relative_to(REPO).as_posix()
                except ValueError:
                    continue
                if not rel.startswith("upstream/") and not rel.startswith(GENERATED):
                    found.add(rel)
    found.update(SCRIPTS)
    missing = [f for f in sorted(found) if not (REPO / f).is_file()]
    if missing:
        raise SystemExit(f"missing: {missing}")
    (HERE / "source-files.txt").write_text("\n".join(sorted(found)) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(found)} files -> {HERE / 'source-files.txt'}")


if __name__ == "__main__":
    main()
