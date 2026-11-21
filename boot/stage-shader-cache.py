"""Prepare and publish the recorded Pokopia build 655360 shader cache."""

import argparse
from ftplib import FTP, error_perm
from hashlib import file_digest, sha256
from pathlib import Path
import re


PROJECT = Path(__file__).resolve().parents[1]
REFERENCE = PROJECT / "frame-reference-655360"
SOURCE = REFERENCE / "shader_cache" / "spirv"
BASELINE = PROJECT / "boot" / "game-boot-r10-shaders"
LOCAL = PROJECT / "boot" / "shader-cache-655360"
REMOTE = "/data/pokopia/shaders"
PENDING = "/data/pokopia/shaders-655360.upload"
FILES = ("shaders.spvpack", "results.tsv")
BLOCK = 1024 * 1024


def manifest(folder):
    with (folder / "results.tsv").open(encoding="utf-8") as source:
        header = next(source).rstrip("\n")
        return header, {row.split("\t", 1)[0]: row.rstrip("\n") for row in source}


def prepare():
    header, source_rows = manifest(SOURCE)
    baseline_header, baseline_rows = manifest(BASELINE)
    if header != baseline_header:
        raise ValueError("Shader manifests have different schemas")
    order = (REFERENCE / "shader_order.txt").read_text(encoding="ascii").splitlines()
    keys = dict.fromkeys([*order, *baseline_rows])
    LOCAL.mkdir(exist_ok=False)
    with (LOCAL / FILES[0]).open("xb") as pack, (LOCAL / FILES[1]).open("x", encoding="utf-8", newline="\n") as index:
        index.write(header + "\tspv_offset\n")
        for key in keys:
            if not re.fullmatch(r"[0-9a-f]{16}", key):
                raise ValueError(f"Invalid shader key: {key!r}")
            row = baseline_rows.get(key) or source_rows[key]
            fields = row.split("\t")
            shader = (BASELINE if key in baseline_rows else SOURCE) / (key + ".spv")
            data = shader.read_bytes()
            if fields[4] != "ok" or len(data) != int(fields[6]) or len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
                raise ValueError(f"Invalid shader cache entry: {key}")
            index.write(f"{row}\t{pack.tell()}\n")
            pack.write(data)
        print(f"Prepared {len(keys):,} shaders, {pack.tell():,} bytes in {LOCAL}")


def exists(ftp, directory):
    try:
        ftp.cwd(directory)
    except error_perm as error:
        if str(error).startswith("550"):
            return False
        raise
    return True


def size(ftp, remote):
    try:
        return ftp.size(remote)
    except error_perm as error:
        if str(error).startswith("550"):
            return None
        raise


def digest(ftp, remote):
    result = sha256()
    ftp.retrbinary(f"RETR {remote}", result.update, blocksize=BLOCK)
    return result.digest()


def file_prefix(source, size):
    with source.open("rb") as stream:
        result = sha256()
        while size:
            data = stream.read(min(size, BLOCK))
            if not data:
                raise ValueError(f"Local shader cache changed: {source}")
            size -= len(data)
            result.update(data)
        return result.digest()


def transfer(ftp, name, directory, published):
    source = LOCAL / name
    length = source.stat().st_size
    with source.open("rb") as stream:
        expected = file_digest(stream, "sha256").digest()
    remote = f"{directory}/{name}"
    current = size(ftp, remote)
    if published or current == length:
        if current != length or digest(ftp, remote) != expected:
            raise ValueError(f"The existing shader cache file differs: {remote}")
    else:
        offset = current or 0
        if offset > length or (offset and digest(ftp, remote) != file_prefix(source, offset)):
            raise ValueError(f"The partial shader cache file differs: {remote}")
        print(f"Uploading {name} at {offset:,}/{length:,} bytes", flush=True)
        with source.open("rb") as stream:
            stream.seek(offset)
            ftp.storbinary(f"STOR {remote}", stream, blocksize=BLOCK, rest=offset or None)
        if size(ftp, remote) != length or digest(ftp, remote) != expected:
            raise ValueError(f"Shader cache verification failed: {remote}")
    print(f"Verified {name} {length:,} bytes {expected.hex()}", flush=True)


def stage(host):
    for name in FILES:
        if not (LOCAL / name).is_file():
            raise ValueError(f"Prepare the local cache first: {LOCAL / name}")
    with FTP() as ftp:
        ftp.connect(host, 2121, timeout=45)
        ftp.login()
        ftp.voidcmd("TYPE I")
        published = exists(ftp, REMOTE)
        directory = REMOTE if published else PENDING
        if not published and not exists(ftp, PENDING):
            ftp.mkd(PENDING)
        for name in FILES:
            transfer(ftp, name, directory, published)
            if not published:
                ftp.sendcmd(f"SITE CHMOD 644 {directory}/{name}")
        if not published:
            ftp.sendcmd(f"SITE CHMOD 751 {PENDING}")
            if exists(ftp, REMOTE):
                raise ValueError(f"The console cache appeared during staging: {REMOTE}")
            ftp.cwd("/data/pokopia")
            ftp.rename(PENDING, REMOTE)
        print(f"Published verified shader cache at {REMOTE}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "stage"))
    parser.add_argument("--host", default="172.30.0.99")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare()
    else:
        stage(args.host)


if __name__ == "__main__":
    main()
