"""Capture Pokopia's PS4 framebuffer or send a temporary virtual controller state."""
import argparse
from ftplib import FTP, error_perm
from io import BytesIO
import math
from pathlib import Path
import struct
import time


DEBUG_PATH = "/data/pokopia/debug"
PROJECT = Path(__file__).resolve().parent
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PAD_NAMES = (
    "A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
    "LEFT", "UP", "RIGHT", "DOWN",
)


def retrieve(ftp, filename, limit):
    output = bytearray()

    def receive(chunk):
        if len(output) + len(chunk) > limit:
            raise ValueError(f"Console file is too large: {filename}")
        output.extend(chunk)

    try:
        ftp.retrbinary(f"RETR {filename}", receive, blocksize=65536)
    except error_perm as error:
        if str(error).startswith("550"):
            return None
        raise
    return bytes(output)


def publish(ftp, name, kind, seq, payload):
    temporary = f"{name}.{seq:016x}.upload"
    data = struct.pack("<4sHHQ", b"PDBG", 1, kind, seq) + payload
    try:
        ftp.storbinary(f"STOR {temporary}", BytesIO(data))
    except error_perm as error:
        if not str(error).startswith("550"):
            raise
        ftp.sendcmd(f"SITE CHMOD 770 {DEBUG_PATH}")
        ftp.storbinary(f"STOR {temporary}", BytesIO(data))
    try:
        ftp.rename(temporary, name)
    except error_perm:
        previous = retrieve(ftp, name, len(data))
        if previous is not None:
            if len(previous) != len(data) or previous[:8] != data[:8]:
                raise
            ftp.delete(name)
        ftp.rename(temporary, name)


def wait_reply(ftp, name, limit, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            ftp.sendcmd(f"MLST {name}")
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
        else:
            data = retrieve(ftp, name, limit)
            if data is None:
                ftp.sendcmd(f"SITE CHMOD 640 {name}")
                data = retrieve(ftp, name, limit)
            if data is not None:
                return data
        time.sleep(0.15)
    raise TimeoutError(f"The PS4 app did not produce {name}")


def capture(ftp, seq, timeout):
    publish(ftp, "capture.req", 1, seq, b"")
    name = f"frame-{seq:016x}.png"
    try:
        data = wait_reply(ftp, name, 64 * 1024 * 1024, timeout)
    except TimeoutError:
        request = struct.pack("<4sHHQ", b"PDBG", 1, 1, seq)
        if retrieve(ftp, "capture.req", len(request)) == request:
            ftp.delete("capture.req")
        raise
    if len(data) < 45 or not data.startswith(PNG_SIGNATURE) or data[12:16] != b"IHDR" or data[-8:-4] != b"IEND":
        raise ValueError("The PS4 returned an incomplete PNG")
    width, height = struct.unpack_from(">II", data, 16)
    directory = PROJECT / "debug-captures"
    directory.mkdir(exist_ok=True)
    path = directory / name
    temporary = path.with_suffix(".part")
    temporary.write_bytes(data)
    temporary.replace(path)
    ftp.delete(name)
    print(f"Captured {width}x{height} to {path}")


def pad(ftp, seq, timeout, names, sticks, hold):
    buttons = sum(1 << PAD_NAMES.index(name) for name in set(names))
    publish(ftp, "pad.req", 2, seq, struct.pack("<QhhhhI", buttons, *sticks, hold))
    name = f"pad-{seq:016x}.ack"
    if wait_reply(ftp, name, 8, timeout) != struct.pack("<Q", seq):
        raise ValueError("The PS4 returned the wrong controller acknowledgment")
    ftp.delete(name)
    print(f"PS4 acknowledged controller state {seq:016x} for {hold} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="172.30.0.99")
    parser.add_argument("--timeout", type=float, default=15.0)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("capture")
    controller = commands.add_parser("pad")
    controller.add_argument("buttons", type=str.upper, nargs="*")
    controller.add_argument("--hold", type=int, default=180, metavar="MS")
    controller.add_argument("--sticks", type=int, nargs=4, default=(0, 0, 0, 0))
    commands.add_parser("release")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be positive")
    if args.command == "pad":
        if any(name not in PAD_NAMES for name in args.buttons):
            parser.error("button names: " + ", ".join(PAD_NAMES))
        if not 1 <= args.hold <= 5000 or any(not -32767 <= axis <= 32767 for axis in args.sticks):
            parser.error("hold must be 1–5000 ms and stick axes must be within -32767 to 32767")

    seq = time.time_ns()
    try:
        with FTP() as ftp:
            ftp.connect(args.host, 2121, timeout=args.timeout)
            ftp.login()
            ftp.cwd(DEBUG_PATH)
            if args.command == "capture":
                capture(ftp, seq, args.timeout)
            elif args.command == "release":
                pad(ftp, seq, args.timeout, (), (0, 0, 0, 0), 0)
            else:
                pad(ftp, seq, args.timeout, args.buttons, args.sticks, args.hold)
    except (OSError, error_perm, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
