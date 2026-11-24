#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the Pokopia PS5 title natively on Windows (no WSL).

Mirrors compile-runtime.sh, compile-game.sh and link-title.sh with Zig's
bundled clang and lld (`zig clang -target x86_64-sie-ps5`, `zig ld.lld`) and a
Windows build of PS5_Vulkan's ps5-native-tool. The game-derived inputs (the
lifted modules, rtld, the module images and the shader cache) come from the
user's own dump; nothing of the game is part of this script.

    python winbuild.py --config winbuild.json [--jobs N] [--only runtime|game|link]

The config names the toolchain and input folders (see DEFAULT_CONFIG).
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

DEFAULT_CONFIG = {
    "zig": "zig.exe",                      # Zig 0.17 (clang 22, lld 22)
    "recomp_include": "recomp/include",    # Pokopia Recomp's nxrecomp headers
    "sdk": "sdk",                          # PS5 payload SDK: target/ and ldscripts/
    "radv_archive": "radv/libvulkan_radeon.ps5.a",
    "radv_include": "radv/include",
    "vk_native": "vk/native",              # PS5_Vulkan tooling/native (app_crt, maps, ldscripts)
    "vk_unwind_script": "vk/ps5-pie-unwind.ld",
    "vk_libc_prx": "vk/libc.prx",
    "vk_stubs": "vk/stubs",                # agc_canary_link_stub.c, agc_driver_canary_link_stub.c
    "radv_local_map": "radv-platform-local.map",
    "link_recipe": "link-recipe.json",     # radv_link_recipe's flags (tools/radv-link.sh)
    "builtins": "libclang_rt.builtins-x86_64.a",
    "opus_lib": "libopus.a",
    "native_tool": "ps5-native-tool.exe",
    "lift": {"main": "lift/main", "sdk": "lift/sdk", "subsdk0": "lift/subsdk0"},
    "rtld_dir": "generated/rtld",          # rtld_7700000000.c, lift-7700000000/funcs.h, ...
    "modules_dir": "generated/modules",    # main.bin sdk.bin subsdk0.bin rtld.bin
    "shader_cache": "shaders",             # shaders.spvpack + results.tsv
    "icon": "",                            # sce_sys/icon0.png (512x512 PNG); default assets/icon0.png
    "work": "work",
    "out": "out",
}

PACKED_UNITS = {"c_042", "c_050", "c_051", "c_052", "c_145"}   # compile-game.sh: main units with packed floats


class Fail(Exception):
    pass


def link_or_copy(source, dest):
    """A hard link when source and dest share an NTFS volume (the shader pack is GBs), else a copy."""
    try:
        os.link(source, dest)
    except OSError:
        shutil.copy2(source, dest)


def log(*parts):
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


class Build:
    def __init__(self, config_path, jobs):
        base = Path(config_path).resolve().parent
        raw = dict(DEFAULT_CONFIG)
        raw.update(json.loads(Path(config_path).read_text(encoding="utf-8")))
        self.cfg = {}
        for key, value in raw.items():
            if value == "":
                self.cfg[key] = None
            elif isinstance(value, dict):
                self.cfg[key] = {k: (base / v).resolve() for k, v in value.items()}
            else:
                self.cfg[key] = (base / value).resolve()
        self.jobs = jobs
        self.work = self.cfg["work"]
        self.work.mkdir(parents=True, exist_ok=True)
        self.zig = str(self.cfg["zig"])
        sdk = self.cfg["sdk"]
        # tooling/prospero-clang18
        # Zig keeps clang's own headers (intrinsics, stddef...) in lib/include.
        self.clang = [self.zig, "clang", "-resource-dir", str(Path(self.zig).parent / "lib"),
                      "-target", "x86_64-sie-ps5", "-fvisibility-nodllstorageclass=default",
                      "-isysroot", str(sdk), "-isystem", str(sdk / "target/include/c++/v1"),
                      "-isystem", str(sdk / "target/include"), "-fno-stack-protector", "-fno-plt",
                      "-femulated-tls", "-fdenormal-fp-math=ieee"]

    # --- helpers -----------------------------------------------------------------
    def run(self, args, what):
        result = subprocess.run(args, capture_output=True, text=True, errors="replace")
        if result.returncode:
            raise Fail(f"{what} failed:\n{(result.stdout + result.stderr)[-4000:]}")
        return result

    def compile_all(self, units, label):
        """units: list of (source, object, flags). Skips objects newer than their source."""
        todo = [u for u in units if not (u[1].exists() and u[1].stat().st_mtime >= u[0].stat().st_mtime)]
        log(f"{label}: {len(units)} units, {len(todo)} to compile, {self.jobs} jobs")
        failed = []

        def one(unit):
            source, obj, flags = unit
            obj.parent.mkdir(parents=True, exist_ok=True)
            tmp = obj.with_suffix(".o.tmp")
            result = subprocess.run(self.clang + flags + ["-c", str(source), "-o", str(tmp)],
                                    capture_output=True, text=True, errors="replace")
            if result.returncode:
                return source, (result.stdout + result.stderr)[-3000:]
            os.replace(tmp, obj)
            return source, None

        done = 0
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            for source, error in pool.map(one, todo):
                done += 1
                if error:
                    failed.append((source, error))
                if done % 20 == 0 or done == len(todo):
                    log(f"{label}: {done}/{len(todo)}")
        if failed:
            source, error = failed[0]
            raise Fail(f"{label}: {len(failed)} units failed; first {source}:\n{error}")

    def archive(self, objects, path):
        tmp = path.with_suffix(".a.tmp")
        if tmp.exists():
            tmp.unlink()
        rsp = path.with_suffix(".rsp")
        rsp.write_text("\n".join(f'"{o.as_posix()}"' for o in objects), encoding="utf-8")
        self.run([self.zig, "ar", "rcs", str(tmp), f"@{rsp}"], f"archive {path.name}")
        os.replace(tmp, path)

    # --- runtime -----------------------------------------------------------------
    def runtime_sources(self):
        text = (REPO / "ps5/build/compile-runtime.sh").read_text(encoding="utf-8")
        def array(name):
            body = re.search(name + r"=\(\s*(.*?)\n\)", text, re.S).group(1)
            items = []
            for token in re.findall(r'"[^"]*"|\S+', body):
                token = token.strip('"').replace("$PINNED", str(REPO / "upstream/switch2-recomp-0.2.0"))
                items.append(Path(token) if os.path.isabs(token) else REPO / token)
            return items
        return array("c_sources"), array("cpp_sources")

    def ps5_flags(self):
        c = self.cfg
        arch = ["-march=znver2", "-O2", "-fPIC", "-funwind-tables", "-ffunction-sections", "-fdata-sections"]
        defs = ["-D__ORBIS__", "-D__PROSPERO__", "-D_BSD_SOURCE=1", "-D_XOPEN_SOURCE=700", "-DPOKOPIA_GUEST_PROBE",
                "-DPOKOPIA_LOADER_PROBE", "-DPOKOPIA_BOOT_PROBE", "-DPOKOPIA_BOOT_GAME", "-DPOKOPIA_GFX",
                "-DPOKOPIA_AUDIO_OUT", "-DPOKOPIA_HWOPUS", "-DPOKOPIA_GAME_BOOT=65"]
        incs = ["-I", str(REPO / "ps5/compat/include"), "-I", str(c["recomp_include"]), "-I", str(REPO / "loader-probe"),
                "-I", str(REPO / "horizon"), "-I", str(REPO / "boot"), "-I", str(REPO / "upstream/opus-1.6.1/include"),
                "-isystem", str(c["radv_include"])]
        return arch, defs, incs

    def build_runtime(self):
        c_sources, cpp_sources = self.runtime_sources()
        arch, defs, incs = self.ps5_flags()
        pinned = REPO / "upstream/switch2-recomp-0.2.0"
        cpp_incs = ["-I", str(REPO / "gfx-bridge/nvn-layout-include"), "-I", str(pinned / "runtime/include"),
                    "-I", str(pinned / "runtime/src/gfx/nvn"), "-I", str(pinned / "runtime/src/gfx/nvn/third_party"),
                    "-I", str(pinned / "tools/tiling/include")]
        obj = self.work / "obj/runtime"
        units = [(s, obj / (s.stem + ".o"), ["-std=gnu11"] + arch + defs + incs + ["-I", str(REPO / "ps5/compat/vkloader")])
                 for s in c_sources]
        units += [(s, obj / (s.stem + ".o"), ["-std=c++20", "-fno-exceptions", "-fno-rtti"] + arch + defs + incs + cpp_incs)
                  for s in cpp_sources]
        self.compile_all(units, "runtime")
        native = self.cfg["vk_native"]
        app = [(native / "app_crt.cpp", self.work / "obj/app_crt.o"),
               (native / "app_cpp_runtime.cpp", self.work / "obj/app_cpp_runtime.o")]
        self.compile_all([(s, o, ["-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-ffunction-sections",
                                   "-fdata-sections"]) for s, o in app], "app runtime")
        stubs = self.work / "stubs"
        stubs.mkdir(parents=True, exist_ok=True)
        for name, source in (("libSceAgc", "agc_canary_link_stub.c"), ("libSceAgcDriver", "agc_driver_canary_link_stub.c")):
            o = self.work / f"obj/{name}_stub.o"
            self.compile_all([(self.cfg["vk_stubs"] / source, o, ["-std=c11", "-O2", "-fPIC"])], name)
            self.run([self.zig, "ld.lld", "-m", "elf_x86_64", "--shared", "-soname", f"{name}.prx",
                      "-o", str(stubs / f"{name}.so"), str(o)], f"{name} stub")

    # --- game modules ------------------------------------------------------------
    def build_game(self):
        flags = ["-std=c++20", "-march=znver2", "-mcx16", "-O2", "-fPIC", "-fno-exceptions", "-fno-rtti",
                 "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", "-I", str(self.cfg["recomp_include"])]
        for name, lift in self.cfg["lift"].items():
            if not lift.is_dir():
                raise Fail(f"missing lifted module {name}: {lift}")
            if name == "main":
                self.run([sys.executable, str(HERE / "patch-lift.py"), str(lift),
                          str(self.work / "obj/game-main/c_112.o")], "patch-lift")
                result = self.run([sys.executable, str(HERE / "prepare-packed-floats.py"),
                                   str(self.cfg["recomp_include"] / "nxrecomp"),
                                   str(self.work / "packed-float32")] + flags, "packed floats")
                packed = Path(result.stdout.strip().splitlines()[-1])
            obj = self.work / f"obj/game-{name}"
            units = []
            for source in sorted(lift.glob("*.cpp")):
                extra = []
                obj_dir = obj
                if name == "main" and source.stem in PACKED_UNITS:
                    extra = ["-DPOKOPIA_PACKED_FLOAT32", "-I", str(packed / "include")]
                    obj_dir = packed / "obj"
                units.append((source, obj_dir / (source.stem + ".o"), extra + flags))
            self.compile_all(units, f"game {name}")
            self.archive([u[1] for u in units], self.work / f"libpokopia-{name}-ps5.a")

    # --- link and title ------------------------------------------------------------
    def link(self):
        c = self.cfg
        recipe = json.loads(c["link_recipe"].read_text(encoding="utf-8"))
        sdk = c["sdk"]
        def remap(arg):
            arg = arg.replace(recipe["sdk"], str(sdk).replace("\\", "/"))
            arg = arg.replace(recipe["archive"], str(c["radv_archive"]).replace("\\", "/"))
            arg = re.sub(r"/[^ ]*/build/radv-platform-local\.map", str(c["radv_local_map"]).replace("\\", "/"), arg)
            arg = re.sub(r"/[^ ]*/libclang_rt\.builtins-x86_64\.a", str(c["builtins"]).replace("\\", "/"), arg)
            arg = arg.replace(recipe["vk"] + "/tooling/psbc/ps5-pie-unwind.ld", str(c["vk_unwind_script"]).replace("\\", "/"))
            arg = arg.replace(recipe["vk"] + "/tooling/native", str(c["vk_native"]).replace("\\", "/"))
            return arg
        script = [remap(a) for a in recipe["script"]]
        inputs = [remap(a) for a in recipe["inputs"]]
        flags = [remap(a) for a in recipe["flags"]]
        stub_sos = sorted((sdk / "target/lib").glob("*.so"))
        objs = sorted((self.work / "obj/runtime").glob("*.o"))
        game_libs = [self.work / f"libpokopia-{n}-ps5.a" for n in ("sdk", "main", "subsdk0")]
        args = (["-m", "elf_x86_64", "-pie", "--eh-frame-hdr", "-z", "max-page-size=0x4000", "-mllvm", "-emulated-tls"]
                + script + ["--eh-frame-hdr"] + flags
                + ["--version-script", str(c["vk_native"] / "app-symbols.map"), "--exclude-libs=ALL", "-e", "_start",
                   "-o", str(self.work / "pokopia.elf"),
                   str(self.work / "obj/app_crt.o"), str(self.work / "obj/app_cpp_runtime.o")]
                + [str(o) for o in objs]
                + [str(self.work / "stubs/libSceAgc.so"), str(self.work / "stubs/libSceAgcDriver.so"), "--start-group"]
                + [str(l) for l in game_libs] + [str(c["opus_lib"]), "--end-group"]
                + inputs + ["--as-needed"] + [str(s) for s in stub_sos] + ["--hash-style=gnu"])
        rsp = self.work / "link.rsp"
        rsp.write_text("\n".join('"' + a.replace("\\", "/") + '"' for a in args), encoding="utf-8")
        log("linking")
        self.run([self.zig, "ld.lld", f"@{rsp}"], "link")
        tool = str(c["native_tool"])
        self.run([tool, "link", "--in", str(self.work / "pokopia.elf"), "--out", str(self.work / "eboot.elf"),
                  "--stub-dir", str(sdk / "target/lib"), "--stub", str(self.work / "stubs/libSceAgc.so"),
                  "--stub", str(self.work / "stubs/libSceAgcDriver.so"), "--module-sdk", "0x02000009",
                  "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"], "module conversion")

    def title(self):
        c = self.cfg
        param = json.loads((REPO / "ps5/title/param.json").read_text(encoding="utf-8"))
        out = c["out"] / param["titleId"]
        if out.exists():
            shutil.rmtree(out)
        for sub in ("sce_sys", "sce_module", "modules", "shaders"):
            (out / sub).mkdir(parents=True)
        self.run([str(c["native_tool"]), "self", "--sign", "--in", str(self.work / "eboot.elf"),
                  "--out", str(out / "eboot.bin"), "--magic", "0x1D3D154F"], "signing")
        shutil.copy2(REPO / "ps5/title/param.json", out / "sce_sys/param.json")
        shutil.copy2(c["icon"] or REPO / "assets/icon0.png", out / "sce_sys/icon0.png")
        shutil.copy2(c["vk_libc_prx"], out / "sce_module/libc.prx")
        for module in ("main", "sdk", "subsdk0", "rtld"):
            shutil.copy2(c["modules_dir"] / f"{module}.bin", out / "modules" / f"{module}.bin")
        for name in ("shaders.spvpack", "results.tsv"):
            link_or_copy(c["shader_cache"] / name, out / "shaders" / name)
        shutil.copy2(REPO / "upstream/opus-1.6.1/COPYING", out / "opus-license.txt")
        self.run([str(c["native_tool"]), "self", "--inspect", "--file", str(out / "eboot.bin")], "inspect")
        log(f"title: {out}")
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", choices=("runtime", "game", "link"))
    a = ap.parse_args()
    b = Build(a.config, a.jobs)
    try:
        if a.only in (None, "runtime"):
            b.build_runtime()
        if a.only in (None, "game"):
            b.build_game()
        if a.only in (None, "link"):
            b.link()
            b.title()
    except Fail as e:
        print("ERROR:", e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
