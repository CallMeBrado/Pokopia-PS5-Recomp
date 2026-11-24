"""Generate the CPU bring-up sources locally from Pokopia 2.0.1 ExeFS."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys

RTLD_HASH = "166d3b75ec5559cd241cbacf03dbf16c267a2cbfc95c33c41ba02d3a03c5616f"
MAIN_BUILD_ID = bytes.fromhex("76a489f3bd3642fa5e18bd83d32c98bacc5f7d0e")
BASES = (0x7700000000, 0x2000000000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exefs", type=Path)
    parser.add_argument("recompiler", type=Path)
    args = parser.parse_args()

    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.recompiler.resolve()))
    from nxrecomp.image import Image
    from nxrecomp.nso import nso_info

    with (args.exefs / "main").open("rb") as source:
        header = source.read(0x54)
    rtld = (args.exefs / "rtld").resolve()
    raw = rtld.read_bytes()
    if (header[:4] != b"NSO0" or header[0x40:] != MAIN_BUILD_ID
            or hashlib.sha256(raw).hexdigest() != RTLD_HASH):
        parser.error("the input must match the Pokopia update 655360 build")

    image = bytearray(0x10008)
    for segment in nso_info(raw)["segments"]:
        start = segment["vaddr"]
        end = start + segment["size"]
        if not segment["sha_ok"] or end > len(image):
            parser.error("rtld segment failed validation")
        image[start:end] = segment["data"]

    output = Path(__file__).resolve().parent / "generated"
    output.mkdir(exist_ok=True)
    with (output / "rtld_image.inc").open("w", encoding="ascii") as source:
        source.write("/* Generated from the local ExeFS. */\n")
        for offset in range(0, len(image), 16):
            row = ", ".join(f"0x{value:02x}" for value in image[offset:offset + 16])
            source.write(f"    {row},\n")

    relr = Image.from_nso(raw, BASES[0], "rtld").relr_offsets()
    if len(relr) != 39 or any(offset + 8 > len(image) for offset in relr):
        parser.error("unexpected RTLD relative relocation table")
    with (output / "rtld_relr.inc").open("w", encoding="ascii") as source:
        source.write("/* Generated from the local ExeFS. */\n")
        for offset in relr:
            addend, = struct.unpack_from("<Q", image, offset)
            source.write(f"    {{ 0x{offset:x}, 0x{addend:x} }},\n")

    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    for base in BASES:
        folder = output / f"lift-{base:x}"
        folder.mkdir(exist_ok=True)
        subprocess.run(
            # -c with an explicit path rather than -m: an embedded Python (-I, ._pth) has no cwd on sys.path.
            [sys.executable, "-c", "import runpy, sys; sys.path.insert(0, '.'); sys.argv[0] = 'nxrecomp'; "
             "runpy.run_module('nxrecomp', run_name='__main__')", "lift", str(rtld), str(folder),
             "--name", "rtld", "--base", hex(base), "--index", "3", "--game", "pkp",
             "--shards", "1", "--jobs", "1"],
            cwd=args.recompiler, env=env, check=True,
        )
        generated = (folder / "c_000.cpp").read_text(encoding="utf-8")
        if '#include <nxrecomp/cxx.h>' not in generated or "guest_unimpl(" in generated:
            parser.error("unexpected translated rtld output")
        # These RTLD routines use only the lifter's C ABI and C helpers.
        generated = generated.replace("#include <nxrecomp/cxx.h>", "#include <nxrecomp/ctx.h>", 1)
        generated = generated.replace('#include "funcs.h"', f'#include "{folder.name}/funcs.h"', 1)
        (output / f"rtld_{base:x}.c").write_text(generated, encoding="utf-8")


if __name__ == "__main__":
    main()

