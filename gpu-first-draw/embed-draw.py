"""Embed the verified first draw from the local 655360 capture."""

from hashlib import sha256
import json
from pathlib import Path
import struct
import sys


def main():
    capture, output = map(Path, sys.argv[1:])
    manifest = json.loads((capture / "manifest.json").read_text(encoding="utf-8"))
    if manifest["shader_cache_keys"] != {"vertex": "821538a4398f85a7", "fragment": "7587f31f1945afaf"}:
        raise ValueError("Unexpected first-draw shader identity")
    inputs = (
        ("draw_vertex_shader", "vertex.spv", 2624),
        ("draw_fragment_shader", "fragment.spv", 9452),
        ("draw_support_ubo", "support-ubo.bin", 0x570),
        ("draw_vertex_ubo", "vertex-ubo.bin", 256),
        ("draw_fragment_ubo", "fragment-ubo.bin", 256),
        ("draw_vertices", "vertices.bin", 96),
        ("draw_indices", "indices.bin", 12),
    )
    text = ["/* Generated from the verified local Pokopia 655360 first draw. */", "#include <stdint.h>"]
    for name, filename, size in inputs:
        raw = (capture / filename).read_bytes()
        if len(raw) != size or manifest["files"][filename] != {"bytes": size, "sha256": sha256(raw).hexdigest()}:
            raise ValueError(f"First draw capture mismatch for {filename}")
        if filename.endswith(".spv"):
            words = struct.unpack(f"<{size // 4}I", raw)
            if words[0] != 0x07230203:
                raise ValueError(f"Invalid SPIR-V header in {filename}")
            ctype, width, values = "uint32_t", 8, words
        else:
            ctype, width, values = "uint8_t", 2, raw
        text.append(f"static const {ctype} {name}[] = {{")
        for start in range(0, len(values), 8):
            text.append("    " + ", ".join(f"0x{value:0{width}x}" for value in values[start:start + 8]) + ",")
        text.append("};")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(text) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
