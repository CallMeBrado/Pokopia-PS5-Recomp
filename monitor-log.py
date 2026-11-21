"""Follow a Pokopia PS4 game-boot log over FTP until the run stops.

Usage: python -I -u monitor-log.py <revision> [--idle 240] [--poll 30] [--max-minutes 90]

Appends new bytes from /data/pokopia-ps4-game-boot-r<rev>.txt to
console-game-boot-r<rev>.log next to this script, and exits when one of these happens:
  stop      a "STOP " line (guest boundary) appeared
  fatal     a "Fatal signal" / "Close requested" line appeared
  idle      the log has not grown for --idle seconds (well above the ~85 s loading stall)
  offline   the PS4 stopped answering FTP for 3 polls in a row (hard lock or closed)
  timeout   --max-minutes elapsed
Then it downloads the Mesa log once and prints a one-line summary.
"""
import argparse
import ftplib
import os
import sys
import time

HOST, PORT = '172.30.0.99', 2121
HERE = os.path.dirname(os.path.abspath(__file__))


def connect():
    ftp = ftplib.FTP()
    ftp.connect(HOST, PORT, timeout=20)
    ftp.login()
    ftp.voidcmd('TYPE I')
    return ftp


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('revision', type=int)
    parser.add_argument('--idle', type=int, default=240)
    parser.add_argument('--poll', type=int, default=30)
    parser.add_argument('--max-minutes', type=int, default=90)
    args = parser.parse_args()

    remote = f'/data/pokopia-ps4-game-boot-r{args.revision}.txt'
    local = os.path.join(HERE, f'console-game-boot-r{args.revision}.log')
    mesa_remote = f'/data/pokopia-ps4-mesa-r{args.revision}.txt'
    mesa_local = os.path.join(HERE, f'console-mesa-r{args.revision}.log')

    # Start fresh: the remote file is rewritten on every launch.
    open(local, 'wb').close()
    offset, last_growth, failures, polls = 0, time.time(), 0, 0
    started = time.time()
    tail = b''
    reason = None
    while reason is None:
        polls += 1
        try:
            ftp = connect()
            try:
                size = ftp.size(remote)
            except ftplib.error_perm:
                size = None  # not created yet
            if size is not None and size < offset:
                # The game was relaunched: restart from the beginning.
                open(local, 'wb').close()
                offset, tail = 0, b''
            if size is not None and size > offset:
                chunks = []
                ftp.retrbinary(f'RETR {remote}', chunks.append, rest=offset)
                data = b''.join(chunks)
                with open(local, 'ab') as out:
                    out.write(data)
                offset += len(data)
                last_growth = time.time()
                scan = tail + data
                if b'\nSTOP ' in scan or scan.startswith(b'STOP '):
                    reason = 'stop'
                elif b'Fatal signal' in scan or b'Close requested' in scan:
                    reason = 'fatal'
                tail = scan[-256:]
            ftp.quit()
            failures = 0
        except (OSError, EOFError, ftplib.Error) as error:
            failures += 1
            print(f'poll {polls}: FTP error {type(error).__name__}: {error}', flush=True)
            if failures >= 3:
                reason = 'offline'
        if reason is None and time.time() - last_growth >= args.idle:
            reason = 'idle'
        if reason is None and time.time() - started >= args.max_minutes * 60:
            reason = 'timeout'
        if polls % 10 == 0 and reason is None:
            print(f'poll {polls}: {offset} bytes, idle {time.time() - last_growth:.0f}s', flush=True)
        if reason is None:
            time.sleep(args.poll)

    if reason != 'offline':
        try:
            ftp = connect()
            with open(mesa_local, 'wb') as out:
                ftp.retrbinary(f'RETR {mesa_remote}', out.write)
            ftp.quit()
        except (OSError, EOFError, ftplib.Error) as error:
            print(f'Mesa log download failed: {error}', flush=True)
    minutes = (time.time() - started) / 60
    print(f'MONITOR END reason={reason} revision={args.revision} bytes={offset} '
          f'minutes={minutes:.1f} log={local}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
