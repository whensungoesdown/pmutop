#!/usr/bin/env python3
"""colors.py SHOT.raw - colours used by each chart in the last frame of a
ptyshot capture, per panel (left half / right half of the screen)."""
import re, sys
raw = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
frame = raw.split("\x1b[H")[-2]
names = {"32": "green", "33": "yellow", "31": "red"}
chart = None
for line in frame.split("\r\n"):
    plain = re.sub(r"\x1b\[[0-9;]*m", "", line)
    for title in ("CPU usage", "IPC", "page fault", "L1I miss", "L1D miss", "L2 miss", "iTLB miss", "dTLB miss"):
        if title in plain[:30]:
            chart = title
    if chart and "│" in plain[:9]:
        half = len(line) // 2
        for side, part in (("left", line[:half]), ("right", line[half:])):
            for code, text in re.findall(r"\x1b\[0;(3[123])[;0-9]*m([^\x1b]*)", part):
                if text.strip(" │+"):
                    print(f"{chart:12s} {side:5s} {names[code]}")
