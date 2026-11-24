#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble the Pokopia PS5 Builder release folder and zip.

    python make-release.py --app <PokopiaPS5Builder.exe build folder> --out <dist folder>
                           [--toolchain-zip toolchain-1.zip] [--python-zip python-...-embed-amd64.zip]

Pass the pinned toolchain ZIP. The release workflow recovers it from the standalone builder.
Without --python-zip, the script fetches the pinned embedded Python ZIP.
The result holds no game data: the builder makes everything game-derived on the user's PC.

    PokopiaPS5Builder/
      PokopiaPS5Builder.exe       the window (.NET Framework 4.8)
      builder/                    pipeline.py, the exFAT writer, the default icon, shader order
      source/                     the port's sources the build compiles (source-files.txt)
      toolchain/                  PS5 SDK, RADV, native tooling (make-toolchain-pack.py)
      python/                     Python 3.13 embeddable (PSF license)
      README.txt, LICENSE
"""
import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
VERSION = (HERE / "VERSION").read_text(encoding="ascii").strip()

PYTHON = ("https://www.python.org/ftp/python/3.13.17/python-3.13.17-embed-amd64.zip",
          "9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38")
TOOLCHAIN_SHA256 = "1a1357e763a6ed6c21a32d33f00cafd7cf08e6a8ed83d723fc1c7801c6447684"

BUILDER_FILES = ["pipeline.py", "shader_order.txt", "default-icon0.png"]

README = """Pokopia PS5 Builder {version}

Builds the PS5 version of Pokopia (PPSA94010) from your own copy of the game.

You need
  - a dump of Pokopia 2.0.1 (update 655360) made with your own console and keys:
      <dump>\\base\\exefs and <dump>\\updates\\655360\\exefs (decrypted NSOs) and
      <dump>\\updates\\655360\\romfs (extracted)
    the same layout Pokopia PC 0.1.1 uses;
  - Windows 10 or 11 (x64), about 45 GB free while it builds, an internet connection for the
    first build (it downloads Pokopia PC 0.1.1's build tools from GitHub, about 170 MB).
    Put the temporary files on an NTFS drive: translating the shaders writes hundreds of thousands
    of small files, which an exFAT drive handles many times slower.

Use
  1. Run PokopiaPS5Builder.exe.
  2. Pick the dump and a local output folder. The builder uses an extracted Control icon when
     it finds one in your dump, with a built-in icon as the fallback. Choose an image to override it.
  3. Choose the PPSA94010 folder, an exFAT image (.exfat), a UFS2 image (.ffpkg), or FTP upload.
     For FTP, start the PS5's FTP server and enter its IP address (default port 2121). The title
     is staged locally, then uploaded to /data/homebrew/PPSA94010 when that folder is free.
  4. Press Build. The first build takes one to three hours, mostly for shader translation.
     Stop at any time. Press Build again to continue where it stopped.

What it does
  Checks the dump, translates the game code (Pokopia PC's lifter), translates all ~99,000 of the
  game's shaders to SPIR-V and packs them, compiles and links the PS5 runtime with Zig's clang
  and the PS5 payload SDK, signs eboot.bin, and adds the game data.

Licences
  The builder and the port: GPL-3.0-or-later (LICENSE). toolchain\\SOURCES.txt lists the
  toolchain's sources and licences; python\\LICENSE.txt is Python's. Downloaded at build time:
  Pokopia PC 0.1.1 (drakolordx7/switch2-recomp, GPL-3.0) and, for .ffpkg, UFS2Tool 4.1
  (SvenGDK/UFS2Tool, BSD-2-Clause).
"""


def fetch(url, digest, cache):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / url.rsplit("/", 1)[1]
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        print("downloading", url, flush=True)
        with urllib.request.urlopen(url, timeout=120) as r:
            path.write_bytes(r.read())
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        sys.exit(f"{url}: SHA-256 mismatch")
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--toolchain-zip", required=True, type=Path)
    ap.add_argument("--python-zip", type=Path)
    ap.add_argument("--launcher-payload", action="store_true",
                    help="build the launcher payload ZIP with the toolchain packaged as a separate resource")
    a = ap.parse_args()
    cache = a.out / "downloads"
    python_zip = a.python_zip or fetch(*PYTHON, cache)
    toolchain_zip = a.toolchain_zip
    with toolchain_zip.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != TOOLCHAIN_SHA256:
            sys.exit(f"{toolchain_zip} does not match the pinned toolchain SHA-256")

    root = a.out / "PokopiaPS5Builder"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)

    for name in ("PokopiaPS5Builder.exe", "PokopiaPS5Builder.exe.config"):
        if (a.app / name).exists():
            shutil.copy2(a.app / name, root / name)
    if not (root / "PokopiaPS5Builder.exe").exists():
        sys.exit(f"{a.app} has no PokopiaPS5Builder.exe")

    builder = root / "builder"
    builder.mkdir()
    for name in BUILDER_FILES:
        shutil.copy2(HERE / name, builder / name)
    shutil.copytree(HERE / "mkpfs_exfat", builder / "mkpfs_exfat", ignore=shutil.ignore_patterns("__pycache__"))

    source = root / "source"
    files = [f for f in (HERE / "source-files.txt").read_text(encoding="utf-8").split("\n") if f]
    for rel in files:
        dest = source / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / rel, dest)

    with zipfile.ZipFile(toolchain_zip) as z:
        if "toolchain/VERSION" not in z.namelist():
            sys.exit(f"{toolchain_zip} is not a toolchain pack")
        if not a.launcher_payload:
            z.extractall(root)
    with zipfile.ZipFile(python_zip) as z:
        z.extractall(root / "python")

    (root / "README.txt").write_text(README.format(version=VERSION), encoding="utf-8", newline="\r\n")
    license_file = REPO / "LICENSE"
    if license_file.exists():
        shutil.copy2(license_file, root / "LICENSE.txt")
        shutil.copy2(license_file, source / "LICENSE")

    filename = "PokopiaPS5Builder-payload.zip" if a.launcher_payload else f"PokopiaPS5Builder-{VERSION}.zip"
    archive = a.out / filename
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(root.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(a.out).as_posix())
    print(f"{archive}: {archive.stat().st_size / 2**20:.1f} MiB, {len(files)} source files")


if __name__ == "__main__":
    main()
