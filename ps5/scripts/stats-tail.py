#!/usr/bin/env python3
"""Print the last N performance stat blocks from the running game's log."""
import ftplib
import io
import sys

HOST, PORT = "172.30.0.212", 2121
KEYS = ("Load stats", "General draw", "Replay timing", "Graphics stats top", "IPC by service")
count = int(sys.argv[1]) if len(sys.argv) > 1 else 1
client = ftplib.FTP()
client.connect(HOST, PORT, timeout=30)
client.login()
data = io.BytesIO()
client.retrbinary("RETR /data/pokopia/ps5-game-boot-r65.txt", data.write)
lines = [l for l in data.getvalue().decode(errors="replace").splitlines() if l.startswith(KEYS)]
for line in lines[-len(KEYS) * count - 1:]:
    print(line[:700])
