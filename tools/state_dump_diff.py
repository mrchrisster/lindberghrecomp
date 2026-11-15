#!/usr/bin/env python3
"""Byte diff of two state dumps (LINDBERGH_STATE_DUMP=<file>@<frame>).

    state_dump_diff.py a.<frame> b.<frame> [elf] [--regions "<lo-hi,!lo-hi,...>"]

Lists the differing guest address ranges, each with the nearest symbol below
it when the game's executable is given (read with `nm -C`). The bytes the
state hash leaves out are left out here too: a guest mutex's host pointer (the
12 bytes after hle_pthread.c's "LGJ1" magic) and, with --regions (the game
profile's state_hash string), the "!" ranges."""
import bisect
import struct
import subprocess
import sys


def load(path):
    out = {}
    with open(path, "rb") as f:
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            lo, hi = struct.unpack("<II", h)
            out[lo] = (hi, f.read(hi - lo))
    return out


def symbols(elf):
    addrs, names = [], []
    try:
        lines = subprocess.run(["nm", "-C", "-n", elf], capture_output=True, text=True).stdout.splitlines()
    except OSError:
        return addrs, names
    for line in lines:
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "bBdDsSgGvV":
            addrs.append(int(parts[0], 16)); names.append(parts[2])
    return addrs, names


def masked(lo, data, excl):
    """bytes the hash ignores, as a set of offsets"""
    out = bytearray(len(data))
    for i in range(0, len(data) - 3, 4):
        if data[i:i + 4] == b"1JGL":
            for k in range(4, 16):
                if i + k < len(data):
                    out[i + k] = 1
    for xlo, xhi in excl:
        for addr in range(max(xlo, lo), min(xhi, lo + len(data))):
            out[addr - lo] = 1
    return out


def main(argv):
    excl = []
    if "--regions" in argv:
        k = argv.index("--regions")
        for item in argv[k + 1].split(","):
            if item.startswith("!") and "-" in item:
                lo, hi = item[1:].split("-")
                excl.append((int(lo, 16), int(hi, 16)))
        argv = argv[:k] + argv[k + 2:]
    if len(argv) < 3:
        print(__doc__.strip()); return 2
    a, b = load(argv[1]), load(argv[2])
    addrs, names = symbols(argv[3]) if len(argv) > 3 else ([], [])
    runs = []
    for lo in sorted(set(a) & set(b)):
        da, db = a[lo][1], b[lo][1]
        ma, mb = masked(lo, da, excl), masked(lo, db, excl)
        i, n = 0, min(len(da), len(db))
        diff = lambda k: da[k] != db[k] and not ma[k] and not mb[k]
        while i < n:
            if diff(i):
                j = i
                while j < n and (diff(j) or any(diff(m) for m in range(j + 1, min(n, j + 16)))):
                    j += 1
                runs.append((lo + i, lo + j, da[i:min(j, i + 16)].hex(), db[i:min(j, i + 16)].hex()))
                i = j
            else:
                i += 1
    for s, e, ha, hb in runs:
        k = bisect.bisect_right(addrs, s) - 1
        sym = f"{names[k]}+{s - addrs[k]:#x}" if k >= 0 else ""
        print(f"{s:#010x}..{e:#010x} ({e - s:6d} B) {sym}\n    a {ha}\n    b {hb}")
    print(f"{len(runs)} differing ranges, {sum(e - s for s, e, _, _ in runs)} bytes")
    return 1 if runs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
