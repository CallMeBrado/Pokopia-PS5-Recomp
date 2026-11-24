#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embed the two package identities and a per-file integrity manifest in the launcher."""
import hashlib
import sys
import zipfile
from pathlib import Path


def digest(stream):
    sha = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        sha.update(chunk)
    return sha.hexdigest()


def main(payload, toolchain, output):
    files = {}
    digests = []
    for source, prefix in ((payload, "PokopiaPS5Builder/"), (toolchain, "toolchain/")):
        with source.open("rb") as stream:
            digests.append(digest(stream))
        with zipfile.ZipFile(source) as archive:
            for entry in archive.infolist():
                if not entry.filename.startswith(prefix):
                    raise ValueError(f"Unexpected {source.name} entry: {entry.filename!r}")
                if entry.is_dir():
                    continue
                rel = entry.filename.removeprefix(prefix) if source == payload else entry.filename
                if not rel or any(c in rel for c in "\r\n\t") or rel.casefold() in files:
                    raise ValueError(f"Invalid or duplicate package path: {rel!r}")
                with archive.open(entry) as data:
                    files[rel.casefold()] = f"{digest(data)}\t{entry.file_size}\t{rel}"
    for required in ("pokopiaps5builder.exe", "builder/pipeline.py", "python/python.exe", "toolchain/version"):
        if required not in files:
            raise ValueError(f"Missing package entry: {required}")
    output.parent.mkdir(parents=True, exist_ok=True)
    contents = ("\n".join(digests + sorted(files.values())) + "\n").encode()
    if not output.is_file() or output.read_bytes() != contents:
        output.write_bytes(contents)


if __name__ == "__main__":
    main(*(Path(arg) for arg in sys.argv[1:]))
