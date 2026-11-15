#!/usr/bin/env python3
"""Compare two runs' state-hash logs (LINDBERGH_STATE_HASH=<every N frames>).

    state_hash_diff.py run_a.log run_b.log

Reads the `[hash] frame N: <hex>` lines of each log (with --semantic the
game's own `[shash]` lines) and reports the first
frame whose hashes differ (exit 1), or that every frame both logs have is
identical (exit 0). With LINDBERGH_STATE_HASH_DETAIL=1 the logs also carry
`[hashd] frame N: <addr>:<hex> ...` lines (one hash per memory chunk); for the
first diverging frame the differing chunks are listed, which says where in
guest memory the two runs went apart.
"""
import re
import sys

HASH = re.compile(r"^\[(?:hash|shash)\] frame (\d+): ([0-9a-fA-F]+)")
KIND = "hash"
DETAIL = re.compile(r"^\[hashd\] frame (\d+): (.*)$")


def read(path):
    hashes, detail = {}, {}
    with open(path, errors="replace") as f:
        for line in f:
            m = HASH.match(line)
            if m and line.startswith("[" + KIND + "]"):
                hashes.setdefault(int(m.group(1)), m.group(2).lower())
                continue
            m = DETAIL.match(line)
            if m:
                d = detail.setdefault(int(m.group(1)), {})
                for item in m.group(2).split():
                    if ":" in item:
                        k, v = item.split(":", 1)
                        d[k] = v
    return hashes, detail


def main(argv):
    global KIND
    if "--semantic" in argv:            # the game's small hash ([shash]) instead of the memory hash
        KIND = "shash"
        argv = [a for a in argv if a != "--semantic"]
    if len(argv) != 3:
        print(__doc__.strip())
        return 2
    (ha, da), (hb, db) = read(argv[1]), read(argv[2])
    common = sorted(set(ha) & set(hb))
    if not common:
        print(f"no common frames ({len(ha)} hashes in {argv[1]}, {len(hb)} in {argv[2]})")
        return 2
    for f in common:
        if ha[f] != hb[f]:
            before = [g for g in common if g < f]
            print(f"first divergence at frame {f} (last identical: {before[-1] if before else 'none'}; "
                  f"{len(before)} of {len(common)} common hashes identical)")
            if f in da and f in db:
                diff = sorted(k for k in set(da[f]) | set(db[f]) if da[f].get(k) != db[f].get(k))
                print(f"  {len(diff)} chunks differ: {' '.join(diff[:40])}{' ...' if len(diff) > 40 else ''}")
            return 1
    print(f"identical: {len(common)} common hashes, frames {common[0]}..{common[-1]}"
          f" ({len(ha)} in {argv[1]}, {len(hb)} in {argv[2]})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
