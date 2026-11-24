"""Build-time patches to the locally generated lift (never committed).

The game's frame limiter (main module, function at 0x710089df20) computes its
target rate as 60.0f divided by a helper that returns 1.0. The port's frame cap
replaces that 60.0f constant (0x42700000, loaded into x8 right after the call
that returns to 0x710089e1d0) with pokopia_frame_limit_bits(), so the game's own
limiter, and the real-time delta it derives, runs at 30, 60 or uncapped.

The native zlib adapter is enabled only for the verified generated main unit.
The optional object path lets incremental builds invalidate that unit when the
adapter or the generated input changes.
"""
import hashlib
import os
import pathlib
import re
import sys
import tempfile

UNIT = "c_050.cpp"
RETURN_ADDRESS = "0x710089e1d0ULL"
CONSTANT = "0x42700000ULL"
MARKER = "/* R96: port frame cap */"
DECLARATION = 'extern "C" uint32_t pokopia_frame_limit_bits(void); /* R96 */\n'


def patch_frame(path):
    data = path.read_bytes()
    newline = b"\r\n" if b"\r\n" in data else b"\n"
    lines = data.replace(b"\r\n", b"\n").decode("utf-8").split("\n")
    if any(MARKER in line for line in lines):
        print(f"patch-lift: {UNIT} already patched")
        return
    sites = [i for i, line in enumerate(lines[:-1])
             if RETURN_ADDRESS in line and CONSTANT in lines[i + 1] and "c->x[8] =" in lines[i + 1]]
    if len(sites) != 1:
        sys.exit(f"patch-lift: {UNIT}: expected one frame-limiter site, found {len(sites)}")
    i = sites[0] + 1
    lines[i] = re.sub(r"\(\s*" + re.escape(CONSTANT) + r"\s*\)", "(pokopia_frame_limit_bits())", lines[i]) + " " + MARKER
    includes = [j for j, line in enumerate(lines) if line.startswith("#include")]
    if not includes:
        sys.exit(f"patch-lift: {UNIT}: no include to declare after")
    lines.insert(includes[-1] + 1, DECLARATION.rstrip("\n"))
    path.write_bytes(newline.join(line.encode("utf-8") for line in lines))
    print(f"patch-lift: {UNIT} frame limiter now uses pokopia_frame_limit_bits()")


INFLATE_UNIT = "c_112.cpp"
INFLATE_SHA256 = "660af526696eca43061231acb11d7de6542ca3a447f5da7eb641ac1e316f9f7b"
HEADER = "native-inflate.h"
INCLUDE = b'#include "native-inflate.h"'
HOOK = b"  if (run_native_inflate(c)) return;"
INCLUDE_AFTER = b'#include "funcs.h"'
HOOK_AFTER = b"  c->fn = 0x7101365360ULL;"


def replace_if_changed(path, data):
    if path.is_file() and path.read_bytes() == data:
        return
    fd, temporary_name = tempfile.mkstemp(dir=path.parent)
    staged = pathlib.Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as temp:
            temp.write(data)
        if path.exists():
            staged.chmod(path.stat().st_mode & 0o777)
        os.replace(staged, path)
    finally:
        staged.unlink(missing_ok=True)


def patch_inflate(lift, obj):
    path = lift / INFLATE_UNIT
    if not path.is_file():
        return
    data = path.read_bytes()
    # Undo our two generated lines before checking the original fingerprint.
    # This also removes the adapter if a previously patched input is changed.
    insertions = [(INCLUDE_AFTER, INCLUDE), (HOOK_AFTER, HOOK)]
    pairs = [re.escape(after) + rb"(\r?\n)" + re.escape(line) + rb"\r?\n" for after, line in insertions]
    if all(re.search(pair, data) for pair in pairs):
        for (after, _), pair in zip(insertions, pairs):
            data = re.sub(pair, lambda m: after + m[1], data, count=1)
    header = b""
    if hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest() == INFLATE_SHA256:
        header = pathlib.Path(__file__).with_name(HEADER).read_bytes()
        replace_if_changed(lift / HEADER, header)
        for after, line in insertions:
            data = re.sub(re.escape(after) + rb"(\r?\n)", lambda m: m[0] + line + m[1], data, count=1)
    else:
        print(f"patch-lift: {INFLATE_UNIT} uses its generated inflater")
    replace_if_changed(path, data)
    if obj is not None:
        # A cached object can be newer than files extracted from a ZIP or rsynced.
        # Keep the exact source/header identity beside the compiled object.
        stamp = obj.with_suffix(".inflate-sha256")
        digest = hashlib.sha256(data + b"\0" + header).hexdigest().encode("ascii")
        if not stamp.is_file() or stamp.read_bytes() != digest:
            obj.unlink(missing_ok=True)
            stamp.parent.mkdir(parents=True, exist_ok=True)
            replace_if_changed(stamp, digest)


def main():
    lift = pathlib.Path(sys.argv[1])
    patch_frame(lift / UNIT)
    patch_inflate(lift, pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None)


if __name__ == "__main__":
    main()
