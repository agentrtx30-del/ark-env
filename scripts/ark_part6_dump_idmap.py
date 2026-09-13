#!/usr/bin/env python3
r"""Dump the FName id->name table to work/idmap.tsv for the C++ runtime.
Run once per game session (any time while ARK is running):
python scripts/ark_part6_dump_idmap.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (
    build_name_table, iter_regions, readable, MEM_PRIVATE, MEM_MAPPED,
)

def main():
    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(
        proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION
    )
    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, 48 * (1 << 30))

    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "idmap.tsv")

    written = 0
    with open(out_path, "w", encoding="utf-8") as f:
        for k in sorted(idmap):
            name = idmap[k]
            if not name:
                continue
            if "\t" in name or "\n" in name or "\r" in name:
                continue
            f.write(f"{k}\t{name}\n")
            written += 1

    print(f"wrote {written} entries to {out_path}")

if __name__ == "__main__":
    main()