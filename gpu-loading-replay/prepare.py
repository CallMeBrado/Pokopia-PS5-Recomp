"""Package the verified local loading-indicator draw inputs."""

import hashlib
import json
from pathlib import Path
import struct
import sys


def main():
    capture, stage = map(Path, sys.argv[1:])
    manifest = json.loads((capture / "manifest.json").read_text(encoding="utf-8"))
    if manifest["frame"] != 540 or manifest["shader_cache_keys"] != {
        "vertex": "a26763a4bdd7b361", "fragment": "2db3327d9fbc5bb3",
    }:
        raise ValueError("Unexpected loading draw capture")
    expected = (
        ("VERTEX_SPV", "vertex.spv", 11844), ("FRAGMENT_SPV", "fragment.spv", 10588),
        ("SUPPORT_UBO", "support-ubo.bin", 1392), ("VERTICES", "vertices.bin", 528),
        ("VS3", "draw3-vs-ubo.bin", 3328), ("FS3", "draw3-fs-ubo.bin", 256),
        ("INDEX3", "draw3-indices.bin", 12), ("BC7_3", "draw3-bc7.bin", 64),
        ("VS4", "draw4-vs-ubo.bin", 3328), ("FS4", "draw4-fs-ubo.bin", 256),
        ("INDEX4", "draw4-indices.bin", 24), ("BC7_4", "draw4-bc7.bin", 2097152),
    )
    payload = bytearray()
    header = ["/* Exact local Pokopia update 655360 loading draws, frame 540. */", "enum {"]
    for label, filename, size in expected:
        raw = (capture / filename).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if len(raw) != size or manifest["files"][filename] != {"bytes": size, "sha256": digest}:
            raise ValueError(f"Input mismatch in {filename}")
        if filename.endswith(".spv") and struct.unpack_from("<I", raw)[0] != 0x07230203:
            raise ValueError(f"Invalid SPIR-V header in {filename}")
        payload.extend(bytes((-len(payload)) % 4))
        header.append(f"    {label}_OFFSET = {len(payload)}, {label}_SIZE = {size},")
        payload.extend(raw)
    header.extend([f"    PAYLOAD_SIZE = {len(payload)},", "};", ""])
    package = stage / "package" / "assets" / "images"
    package.mkdir(parents=True, exist_ok=True)
    (package / "loading-draws.bin").write_bytes(payload)
    (stage / "loading-data.h").write_text("\n".join(header), encoding="ascii")


if __name__ == "__main__":
    main()
