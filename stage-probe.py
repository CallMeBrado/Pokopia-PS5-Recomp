"""Stage a built probe on the PS4 and remove its previous staged installer."""
import argparse
from ftplib import FTP, error_perm
from hashlib import file_digest, sha256
from pathlib import Path
import re


def local_hash(package):
    with package.open("rb") as source:
        return file_digest(source, "sha256").hexdigest()


def remote_hash(ftp, name):
    digest = sha256()
    try:
        ftp.retrbinary(f"RETR {name}", digest.update, blocksize=65536)
    except error_perm as error:
        if error.args[0].startswith("550"):
            return None
        raise
    return digest.hexdigest()


def verify_name(path, parser):
    if not path.is_file() or not re.fullmatch(r"IV0000-BREW(?:4899[0-9]|490[0-9]{2})_00-POKOPIAPROBE[0-9]{4}\.pkg", path.name):
        parser.error(f"expected a locally built Pokopia probe installer: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--replace", type=Path, help="local copy of the previous staged installer")
    parser.add_argument("--host", default="172.30.0.99")
    args = parser.parse_args()
    verify_name(args.package, parser)
    if args.replace:
        verify_name(args.replace, parser)
        if args.replace.name == args.package.name:
            parser.error("the current and previous installers must have different names")

    expected = local_hash(args.package)
    name = args.package.name
    upload = name + ".upload"
    with FTP() as ftp:
        ftp.connect(args.host, 2121, timeout=30)
        ftp.login()
        ftp.cwd("/data/pkg")
        current = remote_hash(ftp, name)
        if current is None:
            if remote_hash(ftp, upload) is not None:
                raise SystemExit(f"An unfinished upload already exists: {upload}")
            with args.package.open("rb") as source:
                ftp.storbinary(f"STOR {upload}", source, blocksize=65536)
            if remote_hash(ftp, upload) != expected:
                raise SystemExit(f"Transfer verification failed: {upload}")
            ftp.rename(upload, name)
        elif current != expected:
            raise SystemExit(f"A different package already exists: {name}")
        print(f"Verified /data/pkg/{name}")

        if args.replace:
            old = args.replace.name
            previous = remote_hash(ftp, old)
            if previous is not None:
                if previous != local_hash(args.replace):
                    raise SystemExit(f"Previous staged package differs from local copy: {old}")
                ftp.delete(old)
                if remote_hash(ftp, old) is not None:
                    raise SystemExit(f"Previous staged package remains visible: {old}")
                print(f"Removed /data/pkg/{old}")
            else:
                print(f"Previous staged package is already absent: {old}")


if __name__ == "__main__":
    main()

