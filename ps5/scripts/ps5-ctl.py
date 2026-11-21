"""Drive the PS5 test loop without anyone at the console.

    python ps5/scripts/ps5-ctl.py payload           # once per PS5 boot: send pokopia-ctl to elfldr (9021)
    python ps5/scripts/ps5-ctl.py ping | status
    python ps5/scripts/ps5-ctl.py launch | kill     # the title (default PPSA94010)
    python ps5/scripts/ps5-ctl.py push              # kill, push eboot, clear diagnostics, launch
    python ps5/scripts/ps5-ctl.py shot [--out f.png]   # screenshot from the running game
    python ps5/scripts/ps5-ctl.py pad A [B ...] [--hold 150] [--lx N --ly N --rx N --ry N]

shot and pad need the game running (they go through its debug mailbox in
/data/pokopia/debug); launch/kill need the pokopia-ctl payload.
"""
import argparse
import ftplib
import io
import os
import socket
import struct
import subprocess
import sys
import time

HOST = os.environ.get("PS5_HOST", "172.30.0.212")
TITLE = "PPSA94010"
CTL_PORT, ELF_PORT, FTP_PORT = 9030, 9021, 2121
HERE = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.join(HERE, "..", "ctl", "pokopia-ctl.elf")
DEBUG_DIR = "/data/pokopia/debug"
DIAGNOSTICS = ["ps5-game-boot-r65.txt", "ps5-prof.txt", "ps5-timeline.txt", "ps5-fsp.txt"]
# libnx HidNpadButton bits
BUTTONS = {"A": 0, "B": 1, "X": 2, "Y": 3, "LS": 4, "RS": 5, "L": 6, "R": 7, "ZL": 8, "ZR": 9,
           "PLUS": 10, "MINUS": 11, "LEFT": 12, "UP": 13, "RIGHT": 14, "DOWN": 15}


def ctl(command, timeout=15):
    with socket.create_connection((HOST, CTL_PORT), timeout=timeout) as sock:
        sock.sendall((command + "\n").encode())
        return sock.recv(256).decode().strip()


def ftp():
    client = ftplib.FTP()
    client.connect(HOST, FTP_PORT, timeout=60)
    client.login()
    return client


def send_payload():
    with open(PAYLOAD, "rb") as fh, socket.create_connection((HOST, ELF_PORT), timeout=15) as sock:
        sock.sendall(fh.read())
    for _ in range(20):
        time.sleep(0.5)
        try:
            return ctl("ping")
        except OSError:
            pass
    return "no reply from pokopia-ctl"


def wait_closed(title, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        if ctl(f"appid {title}").startswith("err"):
            return True
        time.sleep(0.5)
    return False


def put_request(client, name, payload):
    """Upload under a temporary name and rename, so the game never reads half a request."""
    client.cwd(DEBUG_DIR)
    temp = name + ".up"
    client.storbinary(f"STOR {temp}", io.BytesIO(payload))
    client.rename(temp, name)


def shot(out):
    seq = time.time_ns()
    request = b"PDBG" + struct.pack("<HHQ", 1, 1, seq)
    client = ftp()
    put_request(client, "capture.req", request)
    name = f"frame-{seq:016x}.png"
    # The game answers on its next present and writes an uncompressed 4K PNG,
    # which can take well over 20 s while it is loading.
    deadline = time.time() + 120
    while time.time() < deadline:
        time.sleep(1.0)
        listing = client.nlst()
        for stale in listing:
            if stale.startswith("frame-") and stale.endswith(".png") and stale != name:
                try:
                    client.delete(stale)
                except ftplib.all_errors:
                    pass
        if name in listing:
            time.sleep(0.5)  # let the game finish writing
            with open(out, "wb") as fh:
                client.retrbinary(f"RETR {name}", fh.write)
            client.delete(name)
            client.quit()
            return out
    client.quit()
    return None


def pad(names, hold, sticks):
    buttons = 0
    for name in names:
        buttons |= 1 << BUTTONS[name.upper()]
    seq = time.time_ns()
    request = b"PDBG" + struct.pack("<HHQQhhhhI", 1, 2, seq, buttons, *sticks, hold)
    client = ftp()
    put_request(client, "pad.req", request)
    ack = f"pad-{seq:016x}.ack"
    deadline = time.time() + 10
    while time.time() < deadline:
        time.sleep(0.2)
        if ack in client.nlst():
            client.delete(ack)
            client.quit()
            return True
    client.quit()
    return False


def push(title):
    state = ctl(f"appid {title}")
    if state.startswith("ok"):
        print("kill:", ctl(f"kill {title}"))
        if not wait_closed(title):
            sys.exit("title did not close")
        time.sleep(1)
    subprocess.run([sys.executable, os.path.join(HERE, "push-eboot.py")], check=True)
    client = ftp()
    client.cwd("/data/pokopia")
    for name in DIAGNOSTICS:
        try:
            client.delete(name)
        except ftplib.all_errors:
            pass
    client.quit()
    print("launch:", ctl(f"launch {title}"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["payload", "ping", "status", "launch", "kill", "push", "shot", "pad"])
    ap.add_argument("buttons", nargs="*")
    ap.add_argument("--title", default=TITLE)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "logs", "screen.png"))
    ap.add_argument("--hold", type=int, default=150)
    for stick in ("lx", "ly", "rx", "ry"):
        ap.add_argument(f"--{stick}", type=int, default=0)
    args = ap.parse_args()
    if args.command == "payload":
        print(send_payload())
    elif args.command == "ping":
        print(ctl("ping"))
    elif args.command == "status":
        print(ctl(f"appid {args.title}"))
    elif args.command in ("launch", "kill"):
        print(ctl(f"{args.command} {args.title}"))
    elif args.command == "push":
        push(args.title)
    elif args.command == "shot":
        saved = shot(os.path.abspath(args.out))
        print(saved or "no capture (is the game running a build with the mailbox?)")
    elif args.command == "pad":
        ok = pad(args.buttons, args.hold, (args.lx, args.ly, args.rx, args.ry))
        print("acknowledged" if ok else "no acknowledgement")


if __name__ == "__main__":
    main()
