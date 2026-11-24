#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pokopia PS5 Builder: turns the user's own Pokopia 2.0.1 dump into the PS5 title PPSA94010.

Run by PokopiaPS5Builder.exe with the bundled Python, or by hand:

    python -I pipeline.py --dump <dump folder> --out <output folder>
                          [--formats folder,exfat,ffpkg,ftp] [--ps5-ip host[:port]] [--icon icon0.png] [--jobs N] [--keep]

The dump folder holds base/ and updates/655360/, as Pokopia PC 0.1.1 expects: updates/655360/exefs
(decrypted NSOs) and updates/655360/romfs. Nothing of the game ships with the builder; every
game-derived file is made here, from the dump.

Steps (each one is resumable: a step counts once its marker is in <out>/work/done):
  verify    the ExeFS against Pokopia PC 0.1.1's dump_hashes.json; romfs present
  tools     Pokopia PC 0.1.1 (zig, lifter, shader translator) and the switch2-recomp 0.2.0 sources,
            downloaded once into the builder's cache and checked against pinned SHA-256s
  source    a private copy of the port's sources, where the generators write
  lift      main and subsdk0 (switch2-recomp 0.2.0), sdk (Pokopia PC 0.1.1 + the port's graphics sites)
  generate  rtld and the module images (cpu-probe/generate.py, loader-probe/prepare.py)
  shaders   extract_corpus -> spirv_harness --batch -> pack-shaders (zlib), every shader of the game
  build     winbuild.py: compile, link, sign
  title     <out>/PPSA94010: eboot.bin, sce_sys, sce_module, modules, shaders, romfs
  export    the folder, an exFAT image and/or a UFS2 .ffpkg (both for ShadowMountPlus)

Lines on stdout for the app: @progress <step> <done> <total> <text>  |  @log <text>  |  @error <text>
|  @done <path>. A file <out>/work/cancel stops the build (exit 2); running it again continues.
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
from contextlib import closing, contextmanager
import ftplib
import hashlib
import ipaddress
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse
import urllib.request
import uuid
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP = HERE.parent                      # the builder's folder: builder/, source/, toolchain/, python/, cache/
TITLE_ID = "PPSA94010"
FTP_ROOT = "/data/homebrew"
FTP_PORT = 2121
GAME_VERSION = "655360"
FORMAT = 1                             # bump when the work layout changes: older work folders start over

# Downloads, pinned. Pokopia PC (drakolordx7/switch2-recomp, GPL-3.0) provides zig, the lifter, the shader
# translator and its Python; its 0.2.0 sources provide the lifter the port's main/subsdk0 build uses.
DOWNLOADS = {
    "recomp": ("https://github.com/drakolordx7/switch2-recomp/releases/download/v0.1.1/PokopiaRecomp-0.1.1-win64.zip",
               "170493047c51608e570dd081b57e4a1b80b665e54a9b6fdaa071df95a4674258", "PokopiaRecomp-0.1.1-win64.zip"),
    "upstream": ("https://github.com/drakolordx7/switch2-recomp/archive/refs/tags/v0.2.0.tar.gz",
                 "396728c6027c09540afaaf95690228d4b0ce19fcafa6e54bfa33487072b21668", "switch2-recomp-0.2.0.tar.gz"),
    "ufs2tool": ("https://github.com/SvenGDK/UFS2Tool/releases/download/v4.1/win-x64-selfcontained.zip",
                 "e772e1bbef2609c1afb19ae9d5acccc0b62770c88a3494bca525e4116846628b", "UFS2Tool-4.1-win-x64.zip"),
}

# The supported dump (Pokopia 2.0.1, update 655360), as Pokopia PC 0.1.1's dump_hashes.json.
DUMP_VERSION = "2.0.1 (update 655360)"
MAIN_BUILD_ID = "76a489f3bd3642fa5e18bd83d32c98bacc5f7d0e"
DUMP_SHA256 = {
    "main": "8a8ce87b4edfce88c6260da5dd80d6cafc03a761eebc842d7a3476e353a8336f",
    "sdk": "7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2",
    "subsdk0": "2e91ffc8b6f036ffd34fdb3c3dc280dec80956308e34ea866167650842c9f3be",
    "rtld": "166d3b75ec5559cd241cbacf03dbf16c267a2cbfc95c33c41ba02d3a03c5616f",
    "main.npdm": "de024cc0dcdf3480b1fca447344fce0a35f49aaffe842c25e5fb4c6d359d42b4",
}

# name, NSO, guest base, NxModule index, shards, lifter ("upstream" 0.2.0 or "release" 0.1.1)
LIFTS = [
    ("subsdk0", 0x7600000000, 2, 4, "upstream"),
    ("main", 0x7100000000, 0, 230, "upstream"),
    ("sdk", 0x7500000000, 1, 45, "release"),
]
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


def say(kind, *parts):
    text = " ".join(str(p) for p in parts).replace("\r", "").replace("\n", "\\n")
    print(f"@{kind} {text}", flush=True)


def progress(step, done, total, text):
    say("progress", step, done, total, text)


class Fail(Exception):
    pass


class Cancelled(Exception):
    pass


def ftp_endpoint(address):
    text = (address or "").strip()
    error = "Enter the PS5 IP address or hostname, optionally followed by :port (default 2121)."
    if not text or not re.fullmatch(r"[a-zA-Z0-9.:%_\[\]-]+", text):
        raise Fail(error)
    try:
        if text.count(":") > 1 and not text.startswith("["):
            host, port = str(ipaddress.IPv6Address(text)), FTP_PORT
        else:
            parsed = urllib.parse.urlsplit("//" + text)
            host, port = parsed.hostname, parsed.port
            if not host or text.endswith(":") or port == 0 or ("]" in text and not re.fullmatch(r"\[[^\]]+\](?::\d+)?", text)):
                raise ValueError()
            port = FTP_PORT if port is None else port
    except ValueError:
        raise Fail(error) from None
    return host, port


@contextmanager
def ftp_session(endpoint):
    host, port = endpoint
    try:
        with closing(ftplib.FTP()) as ftp:
            ftp.connect(host, port, timeout=30)
            ftp.login()
            yield ftp
    except ftplib.all_errors as exc:
        raise Fail(f"FTP {host}:{port}: {exc}") from exc


def ftp_entry(ftp, name):
    # This PS5 server accepts LIST in the working directory. Some servers
    # return 550 on NLST for an empty directory; a successful LIST settles it.
    try:
        return any(path.rstrip("/").split("/")[-1] == name for path in ftp.nlst())
    except ftplib.error_perm:
        lines = []
        ftp.retrlines("LIST", lines.append)
        for line in lines:
            if line.startswith("total "):
                continue
            parts = line.split(None, 8)
            if len(parts) != 9 or not parts[0].startswith(("-", "d", "l")):
                raise Fail("The FTP server returned an unrecognized directory listing; cannot safely check the destination.")
            if parts[8].split(" -> ", 1)[0] == name:
                return True
        return False


def ftp_destination(ftp, create_root=False):
    try:
        ftp.cwd(FTP_ROOT)
    except ftplib.error_perm:
        ftp.cwd("/data")
        if ftp_entry(ftp, "homebrew"):
            raise Fail(f"FTP access to {FTP_ROOT} was denied.")
        if not create_root:
            return
        ftp.mkd("homebrew")
        ftp.cwd(FTP_ROOT)
    if ftp_entry(ftp, TITLE_ID):
        raise Fail(f"{FTP_ROOT}/{TITLE_ID} already exists on the PS5. The builder cannot replace an existing title.")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def filesystem(path):
    """The file system of the volume holding path ("NTFS", "exFAT", ...), or "" when unknown."""
    if os.name != "nt":
        return ""
    import ctypes
    root = ctypes.create_unicode_buffer(260)
    name = ctypes.create_unicode_buffer(64)
    k32 = ctypes.windll.kernel32
    if not k32.GetVolumePathNameW(str(path), root, 260):
        return ""
    if not k32.GetVolumeInformationW(root, None, 0, None, None, None, name, 64):
        return ""
    return name.value


def free_gib(path):
    return shutil.disk_usage(path).free / 2**30


# --- running tools ------------------------------------------------------------------------------
class Runner:
    """Runs tool processes at below-normal priority; kills them all when the cancel file appears."""

    def __init__(self, cancel_file):
        self.cancel_file = cancel_file
        self.procs = set()
        self.lock = threading.Lock()
        self.cancelled = False
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            if self.cancel_file.exists():
                self.cancelled = True
                with self.lock:
                    for p in list(self.procs):
                        try:
                            p.kill()
                        except OSError:
                            pass
            time.sleep(0.3)

    def check(self):
        if self.cancelled or self.cancel_file.exists():
            self.cancelled = True
            raise Cancelled()

    def run(self, cmd, what, cwd=None, env=None, on_line=None):
        self.check()
        flags = CREATE_NO_WINDOW | BELOW_NORMAL if os.name == "nt" else 0
        p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, cwd=cwd, env=env, creationflags=flags)
        with self.lock:
            self.procs.add(p)
        tail = []
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            tail.append(line)
            del tail[:-40]
            if on_line:
                on_line(line)
        rc = p.wait()
        with self.lock:
            self.procs.discard(p)
        self.check()
        if rc:
            raise Fail(f"{what} failed (exit {rc}):\n" + "\n".join(tail))
        return tail


# --- the build ----------------------------------------------------------------------------------
class Builder:
    def __init__(self, a):
        self.dump = Path(a.dump).resolve()
        self.out = Path(a.out).resolve()
        self.work = Path(a.work).resolve() if a.work else self.out / "work"
        self.formats = [f for f in a.formats.split(",") if f]
        if not self.formats or set(self.formats) - {"folder", "exfat", "ffpkg", "ftp"}:
            raise Fail("Choose at least one valid export format: folder, exfat, ffpkg or ftp.")
        self.ftp_endpoint = ftp_endpoint(a.ps5_ip) if "ftp" in self.formats else None
        self.icon = Path(a.icon).resolve() if a.icon else None
        self.jobs = max(1, a.jobs)
        self.keep = a.keep
        self.cache = Path(a.cache).resolve() if a.cache else APP / "cache"
        self.toolchain = Path(a.toolchain).resolve() if a.toolchain else APP / "toolchain"
        self.source = Path(a.source).resolve() if a.source else APP / "source"
        self.recomp_dir = Path(a.recomp).resolve() if a.recomp else None
        self.shader_pack = Path(a.shader_pack).resolve() if a.shader_pack else None
        self.exefs = self.dump / "updates" / GAME_VERSION / "exefs"
        self.romfs = self.dump / "updates" / GAME_VERSION / "romfs"
        self.src = self.work / "src"
        self.done_dir = self.work / "done"
        self.runner = None
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1")

    # markers
    def is_done(self, step):
        return (self.done_dir / step).exists()

    def mark(self, step):
        self.done_dir.mkdir(parents=True, exist_ok=True)
        (self.done_dir / step).write_text(time.strftime("%Y-%m-%d %H:%M:%S\n"), encoding="ascii")

    def python(self):
        """Pokopia PC 0.1.1's own Python runs the build steps: its lifter needs the lz4 and capstone modules
        that Python carries (site-packages). The builder's bundled Python only runs this script."""
        return [str(self.recomp / "tools/python/python.exe"), "-I"]

    # --- verify -------------------------------------------------------------------------------
    def verify(self):
        if not self.exefs.is_dir():
            raise Fail(f"{self.exefs} was not found. Pick the dump folder that holds base\\ and updates\\{GAME_VERSION}\\ "
                       f"(Pokopia 2.0.1, update {GAME_VERSION}).")
        if not (self.romfs / "LI_C0_R.IDX").is_file():
            raise Fail(f"{self.romfs} has no LI_C0_R.IDX. Extract the update's romfs (decrypted) into "
                       f"updates\\{GAME_VERSION}\\romfs.")
        head = (self.exefs / "main").read_bytes()[:0x60] if (self.exefs / "main").is_file() else b""
        if head[:4] != b"NSO0":
            raise Fail("exefs\\main is not a decrypted NSO. Extract the game files with your own keys first.")
        if head[0x40:0x54].hex() != MAIN_BUILD_ID:
            raise Fail(f"This is a different version of the game (main build ID {head[0x40:0x54].hex()}). "
                       f"The builder supports Pokopia {DUMP_VERSION}.")
        names = list(DUMP_SHA256)
        for i, name in enumerate(names):
            progress("verify", i, len(names), f"Checking your game files: {name}")
            path = self.exefs / name
            if not path.is_file():
                raise Fail(f"exefs\\{name} is missing. Pick the exefs of a complete dump.")
            if sha256(path) != DUMP_SHA256[name]:
                raise Fail(f"exefs\\{name} does not match Pokopia {DUMP_VERSION}. Re-dump the game, or check that "
                           f"update {GAME_VERSION} is installed.")
        progress("verify", len(names), len(names), f"Game files match Pokopia {DUMP_VERSION}")

    # --- downloads ----------------------------------------------------------------------------
    def fetch(self, key):
        url, digest, name = DOWNLOADS[key]
        self.cache.mkdir(parents=True, exist_ok=True)
        path = self.cache / name
        if path.is_file() and sha256(path) == digest:
            return path
        tmp = path.with_suffix(path.suffix + ".part")
        say("log", f"Downloading {url}")
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got = 0
                while True:
                    self.runner.check()
                    block = r.read(1 << 20)
                    if not block:
                        break
                    f.write(block)
                    got += len(block)
                    progress("tools", got >> 20, max(total >> 20, 1), f"Downloading {name}: {got >> 20} MiB")
        except OSError as e:
            raise Fail(f"Downloading {url} failed: {e}. Check the internet connection, or put {name} in {self.cache} "
                       "by hand.")
        if sha256(tmp) != digest:
            tmp.unlink()
            raise Fail(f"{name} from {url} is not the expected file (SHA-256 mismatch).")
        os.replace(tmp, path)
        return path

    def tools(self):
        """Pokopia PC 0.1.1's tools folder and the switch2-recomp 0.2.0 sources, from the cache."""
        if self.recomp_dir:                           # a Pokopia PC 0.1.1 folder given by hand (development)
            self.recomp = self.recomp_dir
        else:
            self.recomp = self.cache / "PokopiaRecomp-0.1.1"
            if not (self.recomp / ".complete").exists():
                archive = self.fetch("recomp")
                progress("tools", 0, 1, "Unpacking Pokopia PC 0.1.1 tools")
                if self.recomp.exists():
                    shutil.rmtree(self.recomp)
                with zipfile.ZipFile(archive) as z:
                    members = [m for m in z.infolist() if wanted_recomp_file(m.filename)]
                    for i, member in enumerate(members):
                        if i % 50 == 0:
                            self.runner.check()
                            progress("tools", i, len(members), f"Unpacking Pokopia PC 0.1.1 tools: {i} of {len(members)} files")
                        z.extract(member, self.cache)
                (self.recomp / ".complete").write_text("ok\n")
        self.upstream_tar = self.fetch("upstream")
        for need in ("tools/python/python.exe", "tools/build/zig/zig.exe", "tools/build/recomp/nxrecomp", "tools/spirv_harness/spirv_harness.exe",
                     "tools/shaders/extract_corpus.py", "tools/build/dump_hashes.json"):
            if not (self.recomp / need).exists():
                raise Fail(f"{self.recomp / need} is missing; delete {self.recomp} and run again.")
        self.zig = self.recomp / "tools/build/zig/zig.exe"
        self.release_recomp = self.recomp / "tools/build/recomp"
        self.env["ZIG_GLOBAL_CACHE_DIR"] = str(self.work / "zig-cache")
        self.env["ZIG_LOCAL_CACHE_DIR"] = str(self.work / "zig-cache")

    # --- source -------------------------------------------------------------------------------
    def make_source(self):
        if self.is_done("source"):
            # Completed work folders can retain older build scripts and runtime inputs.
            game_build = ("ps5/build/patch-lift.py", "ps5/build/native-inflate.h", "ps5/build/winbuild.py")
            runtime_inputs = {
                "boot/r41-cold-check/loader_native.c": ("loader_native",),
                "gfx-bridge/debug_overlay_pixels.h": ("loader_native", "live_present"),
                "gfx-bridge/orbis_memory.c": ("orbis_memory",),
                "gfx-bridge/orbis_memory.h": ("loader_native", "orbis_memory"),
                "horizon/thread.c": ("thread",),
                "horizon/thread.h": ("loader_native", "thread"),
            }

            def current(name):
                shipped, staged = self.source / name, self.src / name
                if not shipped.is_file():
                    return not staged.is_file()
                return staged.is_file() and staged.read_bytes() == shipped.read_bytes()

            outdated = {obj for path, objects in runtime_inputs.items() if not current(path) for obj in objects}
            if not outdated and all(current(name) for name in game_build):
                return
            for obj in outdated:
                (self.work / "build" / "obj" / "runtime" / f"{obj}.o").unlink(missing_ok=True)
            (self.done_dir / "source").unlink()
        progress("source", 0, 1, "Copying the port's sources")
        if self.src.exists():
            shutil.rmtree(self.src)
        shutil.copytree(self.source, self.src)
        with tarfile.open(self.upstream_tar) as t:
            t.extractall(self.src / "upstream", filter="data")
        opus = self.src / "upstream/opus-1.6.1"
        shutil.copytree(self.toolchain / "opus/include", opus / "include")
        shutil.copy2(self.toolchain / "opus/COPYING", opus / "COPYING")
        progress("source", 1, 1, "Sources ready")
        # lift (sdk sites) and generate write into src: a fresh copy needs them again (lifted modules are kept)
        for step in ("lift", "generate", "build"):
            (self.done_dir / step).unlink(missing_ok=True)
        self.mark("source")

    @property
    def upstream_recomp(self):
        return self.src / "upstream/switch2-recomp-0.2.0/recomp"

    # --- lift ---------------------------------------------------------------------------------
    def lift_module(self, name, base, index, shards, lifter, sites=None):
        out = self.work / "lift" / name
        if (out / ".lifted").exists():
            return
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        recomp = self.upstream_recomp if lifter == "upstream" else self.release_recomp
        args = ["lift", str(self.exefs / name), str(out), "--name", name, "--base", hex(base), "--index", str(index),
                "--game", "pkp", "--shards", str(shards), "--jobs", str(self.jobs)]
        if sites:
            args += ["--sites", str(sites)]
        self.runner.run(self.python() + [str(HERE / "pipeline.py"), "--lift-one", str(recomp), json.dumps(args)],
                        f"Translating {name}", cwd=recomp, env=self.env)
        (out / ".lifted").write_text("ok\n")

    def lift(self):
        if self.is_done("lift"):
            return
        sites_dir = self.src / "sdk-bringup"
        graphics_sites = sites_dir / "graphics.sites"
        for i, (name, base, index, shards, lifter) in enumerate(LIFTS):
            progress("lift", i, len(LIFTS), f"Translating the game code: {name} ({i + 1} of {len(LIFTS)})")
            sites = None
            if name == "main":
                sites = self.upstream_recomp / "games/pkp/main.sites"
            elif name == "sdk":
                py = self.python()
                self.runner.run(py + [sites_dir / "worker-site.py", self.exefs / "sdk", self.release_recomp, graphics_sites],
                                "sdk worker sites", env=self.env)
                self.runner.run(py + [sites_dir / "graphics-sites.py", self.exefs / "sdk", self.release_recomp,
                                      graphics_sites, self.src / "gfx-bridge/sdk-graphics.inc"],
                                "sdk graphics sites", env=self.env)
                sites = graphics_sites
            self.lift_module(name, base, index, shards, lifter, sites)
        progress("lift", len(LIFTS), len(LIFTS), "Game code translated")
        self.mark("lift")

    # --- generate -----------------------------------------------------------------------------
    def generate(self):
        if self.is_done("generate"):
            return
        for d in ("gfx-bridge/generated", "cpu-probe/generated", "loader-probe/generated"):
            (self.src / d).mkdir(parents=True, exist_ok=True)
        progress("generate", 0, 3, "Indexing the game data (romfs)")
        self.runner.run(self.python() + [self.src / "gfx-bridge/build-romfs-index.py", self.romfs,
                                         self.src / "gfx-bridge/generated/romfs-layout.inc"], "romfs index", env=self.env)
        progress("generate", 1, 3, "Translating the system loader (rtld)")
        self.runner.run(self.python() + [self.src / "cpu-probe/generate.py", self.exefs, self.upstream_recomp],
                        "rtld", env=self.env)
        progress("generate", 2, 3, "Preparing the module images")
        self.runner.run(self.python() + [self.src / "loader-probe/prepare.py", self.exefs, self.upstream_recomp],
                        "module images", env=self.env)
        progress("generate", 3, 3, "Modules ready")
        self.mark("generate")

    # --- shaders ------------------------------------------------------------------------------
    def shaders(self):
        if self.is_done("shaders"):
            return
        if self.shader_pack:
            pack = self.work / "shaders"
            pack.mkdir(parents=True, exist_ok=True)
            for name in ("shaders.spvpack", "results.tsv"):
                (pack / name).unlink(missing_ok=True)
                try:
                    os.link(self.shader_pack / name, pack / name)
                except OSError:
                    shutil.copy2(self.shader_pack / name, pack / name)
            self.mark("shaders")
            return
        corpus, spirv, pack = self.work / "corpus", self.work / "spirv", self.work / "shaders"
        tools = self.recomp / "tools"
        if not (corpus / ".complete").exists():
            progress("shaders", 0, 3, "Finding the game's shaders")
            if corpus.exists():
                shutil.rmtree(corpus)

            def scan(line):
                parts = line.split()
                if len(parts) == 3 and parts[0] == "scan:":
                    progress("shaders", int(parts[1]), int(parts[2]), f"Finding the game's shaders: {parts[1]} of {parts[2]} files")
            self.runner.run(self.python() + [tools / "shaders/extract_corpus.py", "--romfs", self.romfs, "--main",
                                             self.exefs / "main", "--out", corpus, "--jobs", min(self.jobs, 8), "--progress"],
                            "Finding shaders", env=self.env, on_line=scan)
            (corpus / ".complete").write_text("ok\n")
        if not (spirv / ".complete").exists():
            total = max(1, sum(1 for _ in open(corpus / "unique.tsv", encoding="utf-8")) - 1)
            seen = [0]

            def translated(line):
                seen[0] += 1
                if seen[0] % 50 == 0:
                    progress("shaders", min(seen[0], total), total, f"Translating shaders: {min(seen[0], total)} of {total}")
            # As Pokopia PC 0.1.1's launcher runs it (--no-val: no SPIR-V validator pass).
            self.runner.run([tools / "spirv_harness/spirv_harness.exe", "--batch", corpus, "--out", spirv,
                             "--jobs", self.jobs, "--no-val"], "Translating shaders", env=self.env, on_line=translated)
            (spirv / ".complete").write_text("ok\n")
        progress("shaders", 0, 1, "Packing the shaders")
        order = HERE / "shader_order.txt"
        cmd = self.python() + [self.src / "ps5/build/pack-shaders.py", "--spirv", spirv, "--out", pack, "--jobs", self.jobs]
        if order.exists():
            cmd += ["--order", order]

        def packed(line):
            if line.startswith("packed "):
                done, total = line.split()[1].rstrip(":").split("/")
                progress("shaders", int(done), int(total), f"Packing shaders: {done} of {total}")
        self.runner.run(cmd, "Packing shaders", env=self.env, on_line=packed)
        if not self.keep:
            shutil.rmtree(spirv, ignore_errors=True)
            shutil.rmtree(corpus, ignore_errors=True)
        self.mark("shaders")

    # --- build --------------------------------------------------------------------------------
    def build(self):
        if self.is_done("build"):
            return
        t = self.toolchain
        config = {
            "zig": str(self.zig), "recomp_include": str(self.release_recomp / "include"), "sdk": str(t / "sdk"),
            "radv_archive": str(t / "radv/libvulkan_radeon.ps5.a"), "radv_include": str(t / "radv/include"),
            "vk_native": str(t / "vk/native"), "vk_unwind_script": str(t / "vk/ps5-pie-unwind.ld"),
            "vk_libc_prx": str(t / "vk/libc.prx"), "vk_stubs": str(t / "vk/stubs"),
            "radv_local_map": str(t / "radv-platform-local.map"), "link_recipe": str(t / "link-recipe.json"),
            "builtins": str(t / "libclang_rt.builtins-x86_64.a"), "opus_lib": str(t / "opus/libopus.a"),
            "native_tool": str(t / "ps5-native-tool.exe"),
            "lift": {n: str(self.work / "lift" / n) for n, *_ in LIFTS},
            "modules_dir": str(self.src / "loader-probe/generated"), "shader_cache": str(self.work / "shaders"),
            "icon": str(self.icon_path()), "work": str(self.work / "build"), "out": str(self.out),
        }
        cfg = self.work / "winbuild.json"
        cfg.write_text(json.dumps(config, indent=1), encoding="utf-8")
        counts = re.compile(r"^\S+ (.+?): (\d+)/(\d+)$")

        def line(text):
            # winbuild logs "<time> game main: 40/230" while compiling, then "<time> linking"
            m = counts.match(text)
            if m:
                label, done, total = m.group(1), int(m.group(2)), int(m.group(3))
                progress("build", done, total, f"Compiling {label}: {done} of {total}")
            elif text.endswith(" linking"):
                progress("build", 0, 1, "Linking eboot.bin")
        self.runner.run(self.python() + [self.src / "ps5/build/winbuild.py", "--config", cfg, "--jobs", self.jobs],
                        "Building eboot.bin", env=self.env, on_line=line)
        self.mark("build")

    def icon_path(self):
        if self.icon and self.icon.is_file():
            return self.icon
        return HERE / "default-icon0.png"

    # --- title and exports --------------------------------------------------------------------
    def title(self):
        folder = self.out / TITLE_ID          # winbuild.py wrote eboot.bin, sce_sys, modules and shaders here
        if not (folder / "eboot.bin").is_file():
            (self.done_dir / "build").unlink(missing_ok=True)
            raise Fail(f"{folder} has no eboot.bin; run the build again.")
        icon = self.icon_path()
        dest_icon = folder / "sce_sys/icon0.png"
        if not dest_icon.is_file() or sha256(icon) != sha256(dest_icon):
            dest_icon.parent.mkdir(parents=True, exist_ok=True)
            tmp = dest_icon.with_name(f".icon0-{uuid.uuid4().hex}.tmp")
            try:
                shutil.copyfile(icon, tmp)
                os.replace(tmp, dest_icon)
            finally:
                tmp.unlink(missing_ok=True)
        if self.is_done("title") and (folder / "romfs").is_dir():
            return folder
        shutil.rmtree(folder / "romfs", ignore_errors=True)
        files = sorted(p for p in self.romfs.rglob("*") if p.is_file())
        total = sum(p.stat().st_size for p in files)
        done = 0
        for p in files:
            self.runner.check()
            dest = folder / "romfs" / p.relative_to(self.romfs)
            dest.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(p, dest)            # same NTFS volume as the dump: no second copy of the game data
            except OSError:
                shutil.copyfile(p, dest)
            done += p.stat().st_size
            progress("title", done >> 20, max(total >> 20, 1), f"Copying the game data: {done >> 20} of {total >> 20} MiB")
        self.mark("title")
        return folder

    def export_exfat(self, folder):
        sys.path.insert(0, str(HERE))
        from mkpfs_exfat import write_exfat_image
        from mkpfs_exfat.pbar import Progress
        image = self.out / f"{TITLE_ID}.exfat"
        tmp = image.with_suffix(".exfat.part")
        for p in (image, tmp):
            p.unlink(missing_ok=True)

        def step(phase, done, total):
            self.runner.check()
            progress("exfat", done >> 20, max(total >> 20, 1),
                     f"Writing the exFAT image: {done >> 20} of {total >> 20} MiB")
        write_exfat_image(folder, tmp, progress=Progress(step))
        os.replace(tmp, image)
        return image

    def export_ffpkg(self, folder):
        tool_dir = self.cache / "UFS2Tool-4.1"
        if not (tool_dir / ".complete").exists():
            archive = self.fetch("ufs2tool")
            if tool_dir.exists():
                shutil.rmtree(tool_dir)
            with zipfile.ZipFile(archive) as z:
                z.extractall(tool_dir)
            (tool_dir / ".complete").write_text("ok\n")
        exe = next(tool_dir.rglob("UFS2Tool.exe"), None)
        if not exe:
            raise Fail(f"UFS2Tool.exe is missing from {tool_dir}; delete that folder and run again.")
        image = self.out / f"{TITLE_ID}.ffpkg"
        if image.exists():
            image.unlink()
        progress("ffpkg", 0, 2, "Writing the UFS2 image (.ffpkg)")
        self.runner.run([exe, "newfs", "-D", folder, image], "UFS2Tool newfs", on_line=lambda l: say("log", l))
        progress("ffpkg", 1, 2, "Checking the UFS2 image")
        self.runner.run([exe, "fsck_ufs", "-n", image], "UFS2Tool fsck_ufs", on_line=lambda l: say("log", l))
        progress("ffpkg", 2, 2, "UFS2 image ready")
        return image

    def export_ftp(self, folder):
        required = ("eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png", "sce_module/libc.prx",
                    "shaders/shaders.spvpack", "shaders/results.tsv", "romfs/LI_C0_R.IDX",
                    *(f"modules/{n}.bin" for n in ("main", "sdk", "subsdk0", "rtld")))
        for name in required:
            if not (folder / name).is_file() or not (folder / name).stat().st_size:
                raise Fail(f"Cannot upload the incomplete title: {folder / name} is missing or empty.")
        paths = sorted(folder.rglob("*"))
        total = sum(path.stat().st_size for path in paths if path.is_file())
        stage = f".{TITLE_ID}.upload-{uuid.uuid4().hex}"
        lock = f".{TITLE_ID}.publish-lock"
        created_dirs, attempted_files = [], []
        locked = False
        sent = reported = 0
        size_supported = True

        def uploaded(block):
            nonlocal sent, reported
            self.runner.check()
            sent += len(block)
            if sent - reported >= 8 << 20:
                progress("ftp", sent >> 20, max(1, total >> 20), f"Uploading to PS5: {sent >> 20} of {total >> 20} MiB")
                reported = sent

        try:
            with ftp_session(self.ftp_endpoint) as ftp:
                ftp_destination(ftp, create_root=True)
                self.runner.check()
                ftp.mkd(stage)
                created_dirs.append(stage)
                progress("ftp", 0, max(1, total >> 20), "Uploading the title to the PS5")
                for path in paths:
                    self.runner.check()
                    name = stage + "/" + path.relative_to(folder).as_posix()
                    if path.is_dir():
                        ftp.mkd(name)
                        created_dirs.append(name)
                        continue
                    attempted_files.append(name)
                    with path.open("rb") as stream:
                        expected = path.stat().st_size
                        start = sent
                        ftp.storbinary("STOR " + name, stream, 1 << 20, uploaded)
                    if sent - start != expected:
                        raise Fail(f"The local file changed during FTP upload: {path}.")
                    if size_supported:
                        # STOR selected TYPE I. A permission or missing-file
                        # error is a failed verification, not missing SIZE support.
                        try:
                            actual = ftp.size(name)
                        except ftplib.error_perm as exc:
                            if str(exc)[:3] not in ("500", "502", "504"):
                                raise
                            size_supported = False
                            say("log", "The FTP server does not implement SIZE. Using completed binary transfers.")
                        else:
                            if actual != expected:
                                raise Fail(f"FTP size mismatch for {path.name}: expected {expected} bytes, received {actual}.")
                # Serialize publishers from this app; recheck the final name
                # immediately before the same-parent RNFR/RNTO.
                self.runner.check()
                try:
                    ftp.mkd(lock)
                except ftplib.error_perm as exc:
                    raise Fail(f"Cannot acquire FTP publishing lock {FTP_ROOT}/{lock}: {exc}") from exc
                locked = True
                ftp_destination(ftp)
                self.runner.check()
                ftp.rename(stage, TITLE_ID)
                created_dirs.clear()
                attempted_files.clear()
        finally:
            if created_dirs or locked:
                try:
                    # A cancelled STOR can leave its control stream mid-reply.
                    # Reconnect and touch only paths this attempt created.
                    with ftp_session(self.ftp_endpoint) as clean:
                        clean.cwd(FTP_ROOT)
                        if locked:
                            clean.rmd(lock)
                        for name in reversed(attempted_files):
                            try:
                                clean.delete(name)
                            except ftplib.error_perm as exc:
                                if not str(exc).startswith("550"):
                                    raise
                        for name in reversed(created_dirs):
                            clean.rmd(name)
                except Fail as exc:
                    say("log", f"FTP cleanup needs attention in {FTP_ROOT} ({stage if created_dirs else lock}): {exc}")
        progress("ftp", max(1, total >> 20), max(1, total >> 20), "Title uploaded to the PS5")
        host, port = self.ftp_endpoint
        authority = f"[{host}]" if ":" in host else host
        return f"ftp://{authority}:{port}{FTP_ROOT}/{TITLE_ID}"

    # --- all ----------------------------------------------------------------------------------
    def run(self):
        self.work.mkdir(parents=True, exist_ok=True)
        cancel = self.work / "cancel"
        if cancel.exists():
            cancel.unlink()
        self.runner = Runner(cancel)
        if self.ftp_endpoint:
            say("log", "Checking the PS5 FTP connection and destination")
            with ftp_session(self.ftp_endpoint) as ftp:
                ftp_destination(ftp)
        stamp_path = self.work / "state.json"
        if not self.is_done("verify"):
            self.verify()
            self.mark("verify")
        self.tools()
        stamp = {"format": FORMAT, "main_build_id": MAIN_BUILD_ID, "dump": str(self.dump)}
        old = json.loads(stamp_path.read_text(encoding="utf-8")) if stamp_path.exists() else None
        if old != stamp:
            for d in ("src", "lift", "corpus", "spirv", "shaders", "build", "title"):
                shutil.rmtree(self.work / d, ignore_errors=True)
            for marker in list(self.done_dir.glob("*")) if self.done_dir.exists() else []:
                if marker.name != "verify":
                    marker.unlink()
            stamp_path.write_text(json.dumps(stamp, indent=1), encoding="utf-8")
        fs = filesystem(self.work)
        if fs and fs != "NTFS" and not self.is_done("shaders"):
            say("log", f"The temporary files are on a {fs} drive ({self.work}). Translating the shaders writes hundreds "
                       "of thousands of small files, which is many times slower there than on NTFS: pick a temporary "
                       "folder on an NTFS drive if you can.")
        if free_gib(self.work) < 45 and not self.is_done("shaders"):
            say("log", f"Only {free_gib(self.work):.0f} GiB free for the temporary files; the build needs about 45 GiB "
                       "while it runs (the finished title is about 15 GiB).")
        t0 = time.time()
        self.make_source()
        self.lift()
        self.generate()
        self.shaders()
        self.build()
        folder = self.title()
        outputs = []
        if "exfat" in self.formats:
            outputs.append(self.export_exfat(folder))
        if "ffpkg" in self.formats:
            outputs.append(self.export_ffpkg(folder))
        if self.ftp_endpoint:
            try:
                outputs.append(self.export_ftp(folder))
            except Fail as exc:
                raise Fail(f"{exc} The completed local title is available at {folder} for retry.") from exc
            except Cancelled:
                say("log", f"The completed local title is available at {folder} for retry.")
                raise
        if "folder" in self.formats:
            outputs.insert(0, folder)
        else:
            shutil.rmtree(folder, ignore_errors=True)
            for step in ("build", "title"):
                (self.done_dir / step).unlink(missing_ok=True)
        if not self.keep:
            for d in ("lift", "build", "zig-cache"):
                shutil.rmtree(self.work / d, ignore_errors=True)
            for step in ("lift", "build"):
                (self.done_dir / step).unlink(missing_ok=True)
        say("log", f"Finished in {(time.time() - t0) / 60:.0f} minutes")
        for path in outputs:
            say("done", path)


def wanted_recomp_file(name):
    """The parts of the Pokopia PC 0.1.1 zip the PS5 build uses. Zig needs only its executable and clang's own
    headers (lib/include): the PS5 build compiles against the PS5 SDK, never Zig's libc or libc++ trees."""
    rel = name.split("/", 1)[1] if "/" in name else ""
    if rel in ("LICENSE.txt", "README.txt") or rel.startswith("licenses/"):
        return True
    if rel.startswith("tools/build/zig/"):
        return rel in ("tools/build/zig/zig.exe", "tools/build/zig/LICENSE") or rel.startswith("tools/build/zig/lib/include/")
    return rel.startswith(("tools/build/recomp/", "tools/build/dump_hashes.json", "tools/spirv_harness/", "tools/shaders/",
                           "tools/python/"))


def lift_one(recomp, args):
    """Child process: one lifter run with the given lifter (keeps its process pool out of the main process)."""
    sys.path.insert(0, recomp)
    os.chdir(recomp)
    from nxrecomp.__main__ import main as nx_main
    return nx_main(json.loads(args)) or 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump")
    ap.add_argument("--out")
    ap.add_argument("--formats", default="folder")
    ap.add_argument("--ps5-ip", help="PS5 FTP IP address or hostname, optionally with a port (default 2121)")
    ap.add_argument("--icon")
    ap.add_argument("--work", help="temporary files (default <out>/work), best on an NTFS drive: the shader steps "
                                   "write hundreds of thousands of small files, which exFAT handles very slowly")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--keep", action="store_true", help="keep the intermediate files (lifted code, SPIR-V, objects)")
    ap.add_argument("--cache", help=argparse.SUPPRESS)
    ap.add_argument("--toolchain", help=argparse.SUPPRESS)
    ap.add_argument("--source", help=argparse.SUPPRESS)
    ap.add_argument("--shader-pack", help=argparse.SUPPRESS)   # reuse a packed shaders folder (testing)
    ap.add_argument("--recomp", help="an unpacked Pokopia PC 0.1.1 folder to use instead of downloading it")
    ap.add_argument("--lift-one", nargs=2, help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.lift_one:
        return lift_one(*a.lift_one)
    if not a.dump or not a.out:
        ap.error("--dump and --out are required")
    try:
        Builder(a).run()
        return 0
    except Cancelled:
        say("cancelled", "Stopped. Start again to continue where it stopped.")
        return 2
    except Fail as e:
        say("error", str(e))
        return 1
    except Exception as e:  # an unexpected failure: say what it was
        import traceback
        say("error", f"{type(e).__name__}: {e}\n{traceback.format_exc()}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
