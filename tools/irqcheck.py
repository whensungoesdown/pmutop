#!/usr/bin/env python3
"""irqcheck.py - list the interrupts that lands on given CPUs, by delta.

    sudo python3 tools/irqcheck.py 4 5 -- seconds [command ...]

Takes two snapshots of /proc/interrupts around an optional command (or a
plain sleep) and prints, per IRQ, how many times it was delivered to each
of the watched CPUs during the window.  Columns in /proc/interrupts are
positional: the header line names them, so we index from the header
rather than guessing offsets.
"""
import subprocess
import sys
import time


def read_irqs():
    counts = {}
    names = {}
    with open("/proc/interrupts") as f:
        header = f.readline().split()
        for line in f:
            # Split once: the rest of the line keeps its inner spacing,
            # and every CPU column is a single token.
            parts = line.split()
            if len(parts) < len(header) + 1:
                continue
            irq = parts[0]
            if not irq.endswith(":"):
                continue
            numa = len(header)
            cpu_cols = parts[1:1 + numa]
            vals = {}
            for i, v in enumerate(cpu_cols):
                try:
                    vals[i] = int(v)
                except ValueError:
                    vals[i] = 0
            counts[irq[:-1]] = vals
            names[irq[:-1]] = " ".join(parts[1 + numa:])[:60]
    return counts, names


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cpus = [int(x) for x in sys.argv[1].split(",")]
    rest = sys.argv[2:]
    window = 10.0
    cmd = None
    if rest and rest[0] == "--":
        rest = rest[1:]
    if rest:
        try:
            window = float(rest[0])
            cmd = rest[1:]
        except ValueError:
            cmd = rest

    before, names = read_irqs()
    t0 = time.time()
    if cmd:
        subprocess.run(cmd, check=False)
    else:
        time.sleep(window)
    after, _ = read_irqs()
    dt = time.time() - t0

    rows = []
    for irq, vals in after.items():
        d = {c: vals.get(c, 0) - before.get(irq, {}).get(c, 0) for c in cpus}
        total = sum(d.values())
        if total > 0:
            rows.append((total, irq, d))
    rows.sort(reverse=True)

    print(f"window {dt:.1f} s, CPUs {cpus}")
    for total, irq, d in rows[:20]:
        per = " ".join(f"cpu{c}={d[c]:7d}" for c in cpus)
        rate = total / dt
        print(f"{per}  total={total:7d} ({rate:7.0f}/s)  irq {irq}: {names.get(irq, '')}")
    if not rows:
        print("no interrupts delivered to those CPUs in the window")
    return 0


if __name__ == "__main__":
    sys.exit(main())
