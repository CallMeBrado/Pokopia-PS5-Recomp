#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack the PS5 toolchain the builder links with into toolchain-<version>.zip.

The pack holds no game data and nothing of Sony's: the PS5 payload SDK headers and stub libraries
(mihawk-99/PS5_PayloadSDK), the RADV Vulkan driver archive (mihawk-99/PS5_Mesa), PS5_Vulkan's native
title tooling (app runtime, clean-room libc.prx, link scripts, ps5-native-tool), the compiler-rt
builtins, Opus, and the link recipe. SOURCES.txt names the exact revisions. The release workflow
restores this ZIP from the previous standalone builder and embeds it in the new executable.

    python make-toolchain-pack.py --wintc <staged toolchain folder> --opus <opus-1.6.1 source> --out <zip>
"""
import argparse
import hashlib
import zipfile
from pathlib import Path

VERSION = "1"

# (path in the pack, path in the staged folder)
ENTRIES = [
    ("sdk", "sdk"),
    ("radv/libvulkan_radeon.ps5.a", "radv/libvulkan_radeon.ps5.a"),
    ("radv/include", "radv/include"),
    ("radv/PROVENANCE.txt", "radv/PROVENANCE.txt"),
    ("vk/native", "vk/native"),
    ("vk/stubs", "vk/stubs"),
    ("vk/ps5-pie-unwind.ld", "vk/ps5-pie-unwind.ld"),
    ("vk/libc.prx", "vk/libc.prx"),
    ("ps5-native-tool.exe", "ps5-native-tool.exe"),
    ("libclang_rt.builtins-x86_64.a", "libclang_rt.builtins-x86_64.a"),
    ("opus/libopus.a", "libopus.a"),
    ("link-recipe.json", "link-recipe.json"),
    ("radv-platform-local.map", "radv-platform-local.map"),
]

SOURCES = """Pokopia PS5 Builder toolchain {version}

Built from these public sources (all GPL-3.0-or-later unless noted):

  sdk/                         mihawk-99/PS5_PayloadSDK  b83202be73e930050e00de0f1b4d9ec46c0391bf
                               (as staged by PS5_Vulkan's tools; headers and link stubs only)
  radv/                        mihawk-99/PS5_Mesa        1ba9874f478657ccf6da72ce55f0260164cc0bd0  (MIT)
                               built by PS5_Vulkan tools/build-radv.sh release (radv/PROVENANCE.txt)
  vk/, ps5-native-tool.exe,    mihawk-99/PS5_Vulkan      7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc
  link-recipe.json,            (tooling/native, tooling/psbc; libc.prx is its clean-room library;
  radv-platform-local.map       ps5-native-tool built for Windows from tooling/native)
  libclang_rt.builtins-x86_64.a  LLVM compiler-rt (Apache-2.0 WITH LLVM-exception)
  opus/                        Opus 1.6.1, https://downloads.xiph.org/releases/opus/  (BSD-3-Clause, opus/COPYING)

https://github.com/mihawk-99/PS5_PayloadSDK
https://github.com/mihawk-99/PS5_Mesa
https://github.com/mihawk-99/PS5_Vulkan
"""


def files_under(base, rel):
    path = base / rel
    if path.is_file():
        yield rel, path
    else:
        for p in sorted(path.rglob("*")):
            if p.is_file():
                yield f"{rel}/{p.relative_to(path).as_posix()}", p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wintc", required=True, type=Path)
    ap.add_argument("--opus", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    count = 0
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dest, src in ENTRIES:
            for rel, path in files_under(a.wintc, src):
                z.write(path, "toolchain/" + dest + rel[len(src):])
                count += 1
        for rel, path in files_under(a.opus, "include"):
            z.write(path, "toolchain/opus/" + rel)
            count += 1
        z.write(a.opus / "COPYING", "toolchain/opus/COPYING")
        z.writestr("toolchain/SOURCES.txt", SOURCES.format(version=VERSION))
        z.writestr("toolchain/VERSION", VERSION + "\n")
    digest = hashlib.sha256(a.out.read_bytes()).hexdigest()
    print(f"{a.out}: {count} files, {a.out.stat().st_size / 2**20:.1f} MiB, sha256 {digest}")


if __name__ == "__main__":
    main()
