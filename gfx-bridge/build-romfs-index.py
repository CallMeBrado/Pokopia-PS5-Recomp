#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a small virtual RomFS index over a directory of loose files."""

import argparse
import struct
from dataclasses import dataclass, field
from pathlib import Path


# The layout and hash rules follow Switch2Recomp runtime/src/services/fs_romfs.cpp.
NONE = 0xFFFFFFFF
DATA_START = 0x200


@dataclass
class Node:
    name: bytes
    relative: bytes
    size: int = 0
    entry: int = 0
    data: int = 0
    parent: int = 0
    sibling: int = NONE
    directories: list = field(default_factory=list)
    files: list = field(default_factory=list)


def aligned(value, alignment):
    return (value + alignment - 1) & -alignment


def collect(folder, parent, dirs, files):
    for child in sorted(folder.iterdir(), key=lambda entry: entry.name.encode("utf-8")):
        name = child.name.encode("utf-8")
        relative = parent.relative + b"/" + name if parent.relative else name
        if len(name) > 255 or len(relative) >= 0x300 or b"\\" in name or b":" in name:
            raise ValueError(f"Path cannot be represented in the virtual RomFS: {child}")
        if child.is_symlink() or child.is_junction():
            raise ValueError(f"Symbolic links are unsupported: {child}")
        node = Node(name, relative)
        if child.is_dir():
            parent.directories.append(node)
            dirs.append(node)
            collect(child, node, dirs, files)
        elif child.is_file():
            node.size = child.stat().st_size
            parent.files.append(node)
            files.append(node)
        else:
            raise ValueError(f"Entry is neither a regular file nor a directory: {child}")


def bucket_count(count):
    if count < 3:
        return 3
    if count < 19:
        return count | 1
    while any(count % prime == 0 for prime in (2, 3, 5, 7, 11, 13, 17)):
        count += 1
    return count


def make_table(nodes, directory):
    heads = [NONE] * bucket_count(len(nodes))
    entries = bytearray()
    for node in nodes:
        hashed = node.parent ^ 123456789
        for byte in node.name:
            hashed = ((hashed >> 5) | (hashed << 27)) & 0xFFFFFFFF
            hashed ^= byte
        index = hashed % len(heads)
        if directory:
            first_dir = node.directories[0].entry if node.directories else NONE
            first_file = node.files[0].entry if node.files else NONE
            entries += struct.pack("<6I", node.parent, node.sibling, first_dir, first_file,
                                   heads[index], len(node.name))
        else:
            entries += struct.pack("<IIQQII", node.parent, node.sibling, node.data, node.size,
                                   heads[index], len(node.name))
        heads[index] = node.entry
        entries += node.name.ljust(aligned(len(node.name), 4), b"\0")
    return struct.pack(f"<{len(heads)}I", *heads), entries


def build_layout(root):
    dirs, files = [Node(b"", b"")], []
    collect(root, dirs[0], dirs, files)
    dirs.sort(key=lambda node: node.relative)
    files.sort(key=lambda node: node.relative)
    for nodes, entry_size in ((dirs, 0x18), (files, 0x20)):
        offset = 0
        for node in nodes:
            node.entry = offset
            offset += entry_size + aligned(len(node.name), 4)
        if offset >= 0xFFFFFFF0:
            raise ValueError("Too many RomFS entries")

    data_end = 0
    for node in files:
        node.data = aligned(data_end, 16)
        data_end = node.data + node.size
    for parent in dirs:
        for children in (parent.directories, parent.files):
            for index, node in enumerate(children):
                node.parent = parent.entry
                if index + 1 < len(children):
                    node.sibling = children[index + 1].entry

    tables = (*make_table(dirs, True), *make_table(files, False))
    table_start = aligned(DATA_START + data_end, 4)
    offset = table_start
    header = [0x50]
    for table in tables:
        header.extend((offset, len(table)))
        offset += len(table)
    header.append(DATA_START)
    if offset > (1 << 63) - 1:
        raise ValueError("RomFS size exceeds the signed IStorage range")
    return struct.pack("<10Q", *header), b"".join(tables), table_start, dirs, files


def quoted_path(path):
    return '"' + ''.join(chr(byte) if 32 <= byte < 127 and byte not in b'"\\?'
                         else f"\\{byte:03o}" for byte in path) + '"'


def write_index(output, header, tables, table_start, files):
    lines = ["/* Generated virtual RomFS layout. File contents stay in the configured root. */",
             f"#define ROMFS_FILE_COUNT {len(files)}u",
             f"static const uint64_t romfs_table_start = UINT64_C({table_start});"]
    for name, data in (("header", header), ("tables", tables)):
        lines.append(f"static const uint8_t romfs_{name}[] = {{")
        for index in range(0, len(data), 16):
            lines.append("    " + " ".join(f"0x{byte:02x}," for byte in data[index:index + 16]))
        lines.append("};")
    lines.append("static const RomfsExtent romfs_files[] = {")
    for node in files:
        lines.append(f"    {{ UINT64_C({DATA_START + node.data}), UINT64_C({node.size}), "
                     f"{quoted_path(node.relative)} }},")
    if not files:
        lines.append("    { 0, 0, NULL },")
    lines.append("};")
    output.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    header, tables, table_start, dirs, files = build_layout(args.root)
    write_index(args.output, header, tables, table_start, files)
    print(f"Indexed {len(files)} files and {len(dirs)} directories, "
          f"{sum(node.size for node in files)} data bytes, {table_start + len(tables)} image bytes")


if __name__ == "__main__":
    main()
