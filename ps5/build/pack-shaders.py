#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack translated shaders into the PS5 title's shaders/ folder.

Reads spirv_harness --batch output (<hash>.spv files and results.tsv), and
writes shaders.spvpack and its index results.tsv as gfx-bridge/shader_cache.c
reads them: every "ok" shader, zlib-compressed (spv_zbytes, padded to 4 bytes)
unless --raw, at spv_offset. Shaders named in --order come first (the ones the
game is known to use early), then the rest by hash. --prefer names an existing
pack whose entries win for the keys it has (shaders retranslated for the port).

    python pack-shaders.py --spirv <harness output> --out <title>/shaders [--order shader_order.txt]
                           [--prefer <existing shaders dir>] [--raw] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import re
import sys
import zlib
from pathlib import Path

SPIRV_MAGIC = b"\x03\x02\x23\x07"
KEY = re.compile(r"[0-9a-f]{16}")


def read_index(path):
    with open(path, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        rows = {}
        for line in f:
            fields = line.rstrip("\n").split("\t")
            rows[fields[0]] = fields
    return header, rows


def compress(data):
    packed = zlib.compress(data, 6)
    return packed + b"\0" * (-len(packed) % 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--order", type=Path)
    ap.add_argument("--prefer", type=Path)
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    header, rows = read_index(a.spirv / "results.tsv")
    columns = header[:header.index("glslc_reflection") + 1] if "glslc_reflection" in header else header
    preferred = {}
    if a.prefer:
        prefer_header, prefer_rows = read_index(a.prefer / "results.tsv")
        offset_col, size_col = prefer_header.index("spv_offset"), prefer_header.index("spv_bytes")
        zcol = prefer_header.index("spv_zbytes") if "spv_zbytes" in prefer_header else None
        for key, fields in prefer_rows.items():
            preferred[key] = (fields[:len(columns)], int(fields[offset_col]), int(fields[size_col]),
                              int(fields[zcol]) if zcol is not None else 0)
    status_col, size_col = header.index("status"), header.index("spv_bytes")
    ok = [k for k, f in rows.items() if KEY.fullmatch(k) and f[status_col] in ("ok", "ok-retry")]
    keys = []
    if a.order and a.order.exists():
        keys = [k for k in a.order.read_text(encoding="ascii").split() if k in rows or k in preferred]
    seen = set(keys)
    keys += sorted(k for k in set(ok) | set(preferred) if k not in seen)
    print(f"{len(keys)} shaders ({len(preferred)} from the preferred pack), compress={not a.raw}", flush=True)

    a.out.mkdir(parents=True, exist_ok=True)
    pack_path, index_path = a.out / "shaders.spvpack.tmp", a.out / "results.tsv.tmp"
    prefer_pack = open(a.prefer / "shaders.spvpack", "rb") if preferred else None

    def load(key):
        if key in preferred:
            fields, offset, size, zbytes = preferred[key]
            prefer_pack.seek(offset)
            data = prefer_pack.read(zbytes or size)
            if zbytes:
                data = zlib.decompress(data)
            return key, fields, data
        data = (a.spirv / f"{key}.spv").read_bytes()
        return key, rows[key][:len(columns)], data

    offset = raw_total = 0
    with open(pack_path, "wb") as pack, open(index_path, "w", encoding="utf-8", newline="\n") as index, \
            concurrent.futures.ThreadPoolExecutor(max(1, a.jobs)) as pool:
        index.write("\t".join(columns + ["spv_offset"] + ([] if a.raw else ["spv_zbytes"])) + "\n")
        batch = 512
        for start in range(0, len(keys), batch):
            loaded = [load(k) for k in keys[start:start + batch]]
            for key, fields, data in loaded:
                if data[:4] != SPIRV_MAGIC or len(data) % 4 or int(fields[size_col]) != len(data):
                    sys.exit(f"{key}: not a SPIR-V module of the declared size")
            stored = [d for _, _, d in loaded] if a.raw else list(pool.map(compress, [d for _, _, d in loaded]))
            for (key, fields, data), blob in zip(loaded, stored):
                pack.write(blob)
                extra = [str(offset)] + ([] if a.raw else [str(len(blob))])
                index.write("\t".join(fields + extra) + "\n")
                offset += len(blob)
                raw_total += len(data)
            done = min(start + batch, len(keys))
            print(f"packed {done}/{len(keys)}: {raw_total / 2**30:.2f} GiB -> {offset / 2**30:.2f} GiB", flush=True)
    if prefer_pack:
        prefer_pack.close()
    os.replace(pack_path, a.out / "shaders.spvpack")
    os.replace(index_path, a.out / "results.tsv")
    print("done", a.out)


if __name__ == "__main__":
    main()
