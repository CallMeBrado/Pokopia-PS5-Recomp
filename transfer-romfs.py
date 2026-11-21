"""Transfer and verify the exact Pokopia RomFS used by the PS4 build."""
import argparse
from ftplib import FTP, error_perm
from hashlib import file_digest, sha256
from pathlib import Path, PurePosixPath
import re


PROJECT = Path(__file__).resolve().parent
REMOTE = "/data/pokopia/romfs"
BLOCK = 1024 * 1024
MISSING = "550"


def manifest(root):
    layout = (PROJECT / "gfx-bridge/generated/romfs-layout.inc").read_text(encoding="ascii")
    records = re.findall(r'\{ UINT64_C\(\d+\), UINT64_C\((\d+)\), "([A-Za-z0-9_./-]+)" \}', layout)
    count = int(re.search(r"ROMFS_FILE_COUNT (\d+)u", layout)[1])
    if len(records) != count:
        raise ValueError("Unexpected path encoding in the generated RomFS layout")
    for size, name in records:
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or relative.as_posix() != name:
            raise ValueError(f"Invalid RomFS relative path: {name}")
        source = root.joinpath(*relative.parts)
        if not source.is_file() or source.stat().st_size != int(size) or not source.resolve().is_relative_to(root):
            raise ValueError(f"Wrong or missing RomFS file: {source}")
        yield source, name, int(size)


def ensure_directory(ftp, path, mode):
    try:
        ftp.cwd(path)
    except error_perm as error:
        if not str(error).startswith(MISSING):
            raise
        ftp.mkd(path)
    try:
        ftp.sendcmd(f"SITE CHMOD {mode} {path}")
    except error_perm as error:
        # The PS5 ftpsrv refuses to chmod directories a title created (owner uid 1).
        print(f"note: could not chmod {path}: {error}", flush=True)


def size_on_console(ftp, name):
    try:
        return ftp.size(name)
    except error_perm as error:
        if str(error).startswith(MISSING):
            return None
        raise


def remote_digest(ftp, name):
    digest = sha256()
    ftp.retrbinary(f"RETR {name}", digest.update, blocksize=BLOCK)
    return digest.digest()


def verify(ftp, name, size, digest):
    if size_on_console(ftp, name) != size or remote_digest(ftp, name) != digest:
        raise ValueError(f"PS4 verification failed for {name}")


def transfer(ftp, source, relative, size):
    name = f"{REMOTE}/{relative}"
    with source.open("rb") as stream:
        digest = file_digest(stream, "sha256").digest()
    if size_on_console(ftp, name) is not None:
        verify(ftp, name, size, digest)
    else:
        temporary = name + ".upload"
        existing = size_on_console(ftp, temporary)
        if existing is not None and existing > size:
            raise ValueError(f"Unexpected partial transfer size for {relative}")
        if existing:
            with source.open("rb") as stream:
                prefix = sha256()
                remaining = existing
                while remaining:
                    block = stream.read(min(remaining, BLOCK))
                    if not block:
                        raise ValueError(f"Local RomFS changed while verifying {relative}")
                    prefix.update(block)
                    remaining -= len(block)
            if prefix.digest() != remote_digest(ftp, temporary):
                raise ValueError(f"Partial transfer does not match the local file: {relative}")
        offset = existing or 0
        if offset < size or existing is None:
            print(f"Uploading {relative} ({offset:,}/{size:,} bytes)", flush=True)
            uploaded = offset
            next_report = offset + 256 * BLOCK

            def progress(block):
                nonlocal uploaded, next_report
                uploaded += len(block)
                if uploaded >= next_report:
                    print(f"  {uploaded:,}/{size:,} bytes sent", flush=True)
                    next_report += 256 * BLOCK

            with source.open("rb") as stream:
                stream.seek(offset)
                ftp.storbinary(f"STOR {temporary}", stream, blocksize=BLOCK, callback=progress,
                               rest=offset or None)
        verify(ftp, temporary, size, digest)
        ftp.rename(temporary, name)
    ftp.sendcmd(f"SITE CHMOD 644 {name}")
    print(f"Verified {relative} {size} {digest.hex()}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("G:/pokopia_dump_v2-0-1/updates/655360/romfs"))
    parser.add_argument("--host", default="172.30.0.99")
    args = parser.parse_args()
    try:
        files = list(manifest(args.source.resolve(strict=True)))
        with FTP() as ftp:
            ftp.connect(args.host, 2121, timeout=45)
            ftp.login()
            ftp.sendcmd("TYPE I")
            ensure_directory(ftp, "/data/pokopia", "777")
            directories = {REMOTE}
            for _, relative, _ in files:
                parts = PurePosixPath(relative).parts[:-1]
                for count in range(1, len(parts) + 1):
                    directories.add(f"{REMOTE}/{'/'.join(parts[:count])}")
            for directory in sorted(directories, key=lambda item: (item.count("/"), item)):
                ensure_directory(ftp, directory, "755")
            for source, relative, size in files:
                transfer(ftp, source, relative, size)
        print(f"Verified {len(files)} files, {sum(size for _, _, size in files):,} bytes at {REMOTE}")
    except (OSError, error_perm, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
