#!/usr/bin/env python3
"""ptyshot.py - run a full-screen program in a pseudo-terminal of a given
size, feed it keys at given times, and save the last screen it drew as
plain text (and the raw byte stream) for inspection.

    ptyshot.py ROWS COLS SECONDS OUT_PREFIX -- command ...
"""
import os, pty, re, select, struct, sys, time, fcntl, termios

rows, cols, secs, out = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
cmd = sys.argv[sys.argv.index("--") + 1:]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(cmd[0], cmd)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
raw = b""
end = time.time() + secs
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            raw += os.read(fd, 65536)
        except OSError:
            break
os.write(fd, b"q")
time.sleep(1)
try:
    while True:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            break
        raw += os.read(fd, 65536)
except OSError:
    pass
open(out + ".raw", "wb").write(raw)
txt = raw.decode("utf-8", "replace")
frames = txt.split("\x1b[H")
last = frames[-2] if len(frames) > 2 else frames[-1]   # the last full frame
plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", last).replace("\r", "")
open(out + ".txt", "w").write(plain)
print(plain)
