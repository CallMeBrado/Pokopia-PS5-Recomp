"""Prepare exact-build Pokopia NSO images for the desktop RTLD probe."""

import argparse
import hashlib
from pathlib import Path
import re
import struct
import sys

MODULES = (
    ("main", 0x7100000000, "8a8ce87b4edfce88c6260da5dd80d6cafc03a761eebc842d7a3476e353a8336f"),
    ("sdk", 0x7500000000, "7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2"),
    ("subsdk0", 0x7600000000, "2e91ffc8b6f036ffd34fdb3c3dc280dec80956308e34ea866167650842c9f3be"),
    ("rtld", 0x7700000000, "166d3b75ec5559cd241cbacf03dbf16c267a2cbfc95c33c41ba02d3a03c5616f"),
)


def page_end(address):
    return (address + 0xfff) & ~0xfff


def write_process_profile(exefs, output):
    raw = (exefs / "main.npdm").read_bytes()
    digest = "de024cc0dcdf3480b1fca447344fce0a35f49aaffe842c25e5fb4c6d359d42b4"
    if hashlib.sha256(raw).hexdigest() != digest or raw[:4] != b"META":
        raise ValueError("main.npdm does not match Pokopia update 655360")

    # META points to ACI0, whose KAC lists the process thread capabilities.
    aci, aci_size = struct.unpack_from("<II", raw, 0x70)
    if aci_size < 0x38 or aci + aci_size > len(raw) or raw[aci:aci + 4] != b"ACI0":
        raise ValueError("invalid NPDM ACI0 section")
    offset, size = struct.unpack_from("<II", raw, aci + 0x30)
    if offset + size > aci_size or size % 4:
        raise ValueError("invalid NPDM kernel capabilities")
    flags = [word for word, in struct.iter_unpack("<I", raw[aci + offset:aci + offset + size])
             if word & 0xF == 7]
    if len(flags) != 1:
        raise ValueError("expected one NPDM thread capability descriptor")
    flags = flags[0]
    low_priority, high_priority = sorted(((flags >> 4) & 63, (flags >> 10) & 63))
    low_core, high_core = (flags >> 16) & 255, flags >> 24
    priority, core = raw[0xE:0x10]
    system_resource_size, _, stack_size = struct.unpack_from("<III", raw, 0x14)
    if (not low_priority <= priority <= high_priority or not low_core <= core <= high_core < 64
            or not stack_size or stack_size % 0x4000):
        raise ValueError("invalid NPDM startup thread configuration")
    core_mask = ((1 << (high_core - low_core + 1)) - 1) << low_core
    priority_mask = ((1 << (high_priority - low_priority + 1)) - 1) << low_priority
    profile = (
        f"#define POKOPIA_MAIN_PRIORITY {priority}u\n"
        f"#define POKOPIA_MAIN_CORE {core}u\n"
        f"#define POKOPIA_MAIN_STACK_SIZE {stack_size:#x}u\n"
        f"#define POKOPIA_CORE_MASK UINT64_C({core_mask:#x})\n"
        f"#define POKOPIA_PRIORITY_MASK UINT64_C({priority_mask:#x})\n"
        f"#define POKOPIA_SYSTEM_RESOURCE_SIZE UINT64_C({system_resource_size:#x})\n"
    )
    (output / "process.inc").write_text(profile, encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exefs", type=Path)
    parser.add_argument("recompiler", type=Path)
    args = parser.parse_args()

    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.recompiler.resolve()))
    from nxrecomp.image import Image
    from nxrecomp.nso import nso_info

    probe = Path(__file__).resolve().parent
    output = probe / "generated"
    output.mkdir(exist_ok=True)
    write_process_profile(args.exefs, output)
    entries = []
    for name, base, digest in MODULES:
        raw = (args.exefs / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            parser.error(f"{name} does not match the exact Pokopia 2.0.1 update 655360 build")
        info = nso_info(raw)
        if name == "sdk":
            sdk = Image.from_nso(raw, base, name)
            start_name = "_ZN2nn4init5StartEmmPFvvES2_S2_"
            start_addresses = [base + value for symbol, _, _, section, value, _ in sdk.symbols()
                               if symbol == start_name and section]
            if len(start_addresses) != 1:
                parser.error("could not identify a unique SDK nn::init::Start export")
            (output / "sdk_start.inc").write_text(f"{start_addresses[0]:#x}ULL\n", encoding="ascii")

        segments = info["segments"]
        if not all(segment["sha_ok"] for segment in segments):
            parser.error(f"{name} failed NSO segment integrity verification")

        image_size = segments[-1]["vaddr"] + segments[-1]["size"]
        memory_size = page_end(image_size + info["bss"])
        image = bytearray(image_size)
        regions = []
        for index, segment in enumerate(segments):
            start = segment["vaddr"]
            end = start + segment["size"]
            image[start:end] = segment["data"]
            region_end = memory_size if index == 2 else page_end(end)
            state, permission = ((3, 5), (3, 1), (4, 3))[index]
            regions.append(f"{{ {start:#x}, {region_end:#x}, {state}, {permission} }}")

        (output / f"{name}.bin").write_bytes(image)
        entries.append(
            f'    {{ "{name}", {base:#x}ULL, {image_size:#x}, {memory_size:#x},'
            f' {{ {", ".join(regions)} }} }},\n'
        )

    (output / "layout.inc").write_text("".join(entries), encoding="ascii")
    decls = probe.parent / "cpu-probe/generated/lift-7700000000/funcs.h"
    addresses = re.findall(r"^void f_([0-9a-f]+)\(", decls.read_text(encoding="ascii"), re.M)
    if not addresses:
        parser.error(f"no translated RTLD functions found in {decls}")
    (output / "rtld_functions.inc").write_text(
        "".join(f"    {{ 0x{address}ULL, f_{address} }},\n" for address in addresses),
        encoding="ascii",
    )


if __name__ == "__main__":
    main()


