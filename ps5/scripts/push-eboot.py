"""Upload just eboot.bin of a built title and clear its previous boot log.

    python ps5/scripts/push-eboot.py [PPSA94010] [--log ps5-game-boot-r65.txt]
"""
import argparse
import ftplib
import os

HOST = os.environ.get("PS5_HOST", "172.30.0.212")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("title", nargs="?", default="PPSA94010")
    ap.add_argument("--log", default="ps5-game-boot-r65.txt")
    args = ap.parse_args()
    local = os.path.join(os.path.dirname(__file__), "..", "dist", args.title, "eboot.bin")
    ftp = ftplib.FTP()
    ftp.connect(HOST, 2121, timeout=60)
    ftp.login()
    ftp.cwd(f"/data/homebrew/{args.title}")
    with open(local, "rb") as fh:
        ftp.storbinary("STOR eboot.bin", fh, blocksize=1 << 20)
    remote, size = ftp.size("eboot.bin"), os.path.getsize(local)
    print(f"eboot.bin remote={remote} local={size} {'ok' if remote == size else 'MISMATCH'}")
    ftp.cwd("/data/pokopia")
    try:
        ftp.delete(args.log)
        print(f"cleared /data/pokopia/{args.log}")
    except ftplib.error_perm:
        pass
    ftp.quit()


if __name__ == "__main__":
    main()
