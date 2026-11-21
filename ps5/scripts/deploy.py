"""Upload a PPSA title folder to the PS5 over FTP (etaHEN/ftpsrv on port 2121).

    python ps5/scripts/deploy.py ps5/dist/PPSA94003
    python ps5/scripts/deploy.py --list /data/homebrew
    python ps5/scripts/deploy.py --get /data/pokopia/ps5-probe.txt
"""
import argparse
import ftplib
import os
import posixpath
import sys

HOST = os.environ.get("PS5_HOST", "172.30.0.212")
PORT = int(os.environ.get("PS5_FTP_PORT", "2121"))


def connect():
    ftp = ftplib.FTP()
    ftp.connect(HOST, PORT, timeout=30)
    ftp.login()
    return ftp


def mkdirs(ftp, path):
    parts = [p for p in path.split("/") if p]
    cur = ""
    for p in parts:
        cur += "/" + p
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass


def upload(ftp, local_dir, remote_root):
    title = os.path.basename(os.path.normpath(local_dir))
    remote_dir = posixpath.join(remote_root, title)
    total = 0
    for dirpath, _, files in os.walk(local_dir):
        rel = os.path.relpath(dirpath, local_dir).replace("\\", "/")
        rdir = remote_dir if rel == "." else posixpath.join(remote_dir, rel)
        mkdirs(ftp, rdir)
        for name in files:
            local = os.path.join(dirpath, name)
            size = os.path.getsize(local)
            with open(local, "rb") as fh:
                ftp.storbinary(f"STOR {posixpath.join(rdir, name)}", fh, blocksize=1 << 20)
            remote_size = ftp.size(posixpath.join(rdir, name))
            status = "ok" if remote_size == size else f"SIZE MISMATCH remote={remote_size}"
            print(f"  {posixpath.join(rdir, name)}  {size} bytes  {status}")
            total += size
    print(f"uploaded {total} bytes to {remote_dir}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("title_dir", nargs="?")
    ap.add_argument("--remote-root", default="/data/homebrew")
    ap.add_argument("--list")
    ap.add_argument("--get")
    args = ap.parse_args()
    ftp = connect()
    if args.list:
        ftp.cwd(args.list)  # this ftpsrv rejects LIST <path>
        ftp.retrlines("LIST")
    elif args.get:
        chunks = []
        ftp.cwd(posixpath.dirname(args.get))  # this ftpsrv rejects absolute RETR paths
        ftp.retrbinary(f"RETR {posixpath.basename(args.get)}", chunks.append)
        sys.stdout.write(b"".join(chunks).decode("utf-8", "replace"))
    elif args.title_dir:
        upload(ftp, args.title_dir, args.remote_root)
    else:
        ap.error("nothing to do")
    ftp.quit()


if __name__ == "__main__":
    main()
