"""Embed locally generated shaders and vertex constants from the pinned 327680 dump."""
import argparse
import struct
from pathlib import Path


def words(path):
    raw = path.read_bytes()
    if len(raw) < 20 or raw[:4] != b"\x03\x02\x23\x07":
        raise ValueError(f"{path} is not little-endian SPIR-V")
    return struct.unpack(f"<{len(raw) // 4}I", raw)


def write_array(output, name, values):
    output.write(f"static const uint32_t {name}[] = {{\n")
    for offset in range(0, len(values), 8):
        output.write("    " + ", ".join(f"0x{v:08x}u" for v in values[offset:offset + 8]) + ",\n")
    output.write("};\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shader_work", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    corpus = args.shader_work / "corpus" / "rdb"
    ctrl = (args.shader_work / "corpus-ctl" / "71912c95f9c9e445.ctl").read_bytes()
    program = (corpus / "e9e6defca60e9dd5-stage0-ca882d0bd9cb5df8.program.bin").read_bytes()
    end, size, offset = struct.unpack_from("<III", ctrl, 0x38)
    if (end, size, offset) != (0x200, 48, 0x200):
        raise ValueError("Unexpected vertex program control layout")
    c1 = struct.unpack_from("<12I", program, offset)
    expected = (-1.0, -1.0, 0.0, 1.0, 3.0, -1.0, 0.0, 1.0, -1.0, 3.0, 0.0, 1.0)
    if struct.unpack_from("<12f", program, offset) != expected:
        raise ValueError("Unexpected fullscreen triangle constants")
    source = Path(__file__).parent
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii") as output:
        output.write("/* Generated from the locally translated 327680 fixture. */\n")
        write_array(output, "composite_vert", words(source / "pokopia-composite.vert.spv"))
        write_array(output, "composite_frag", words(source / "pokopia-composite.frag.spv"))
        write_array(output, "vertex_c1", c1)


if __name__ == "__main__":
    main()
