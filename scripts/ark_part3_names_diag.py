#!/usr/bin/env python3
r"""Diagnostic: raw-search known strings, dump surrounding bytes. Local/singleplayer only."""
import argparse, ctypes, ctypes.wintypes as wt, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3 import get_live_world

kernel32 = ctypes.windll.kernel32
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
MAX_ADDR = 0x7FFFFFFFFFFF


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_uint64),
        ("AllocationBase", ctypes.c_uint64),
        ("AllocationProtect", wt.DWORD),
        ("PartitionId", wt.WORD),
        ("RegionSize", ctypes.c_uint64),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


kernel32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]


def iter_regions(handle):
    addr = 0
    while addr < MAX_ADDR:
        mbi = MBI()
        if not kernel32.VirtualQueryEx(handle, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        if mbi.RegionSize == 0:
            break
        yield mbi
        addr = mbi.BaseAddress + mbi.RegionSize


def readable(mbi):
    return (mbi.State == MEM_COMMIT
            and not (mbi.Protect & PAGE_GUARD)
            and mbi.Protect not in (0, PAGE_NOACCESS))


PATTERNS = [
    ("A_Byte", re.compile(rb"ByteProperty")),
    ("W_Byte", re.compile("ByteProperty".encode("utf-16-le"))),
    ("A_Int", re.compile(rb"IntProperty")),
    ("W_Int", re.compile("IntProperty".encode("utf-16-le"))),
    ("A_None", re.compile(rb"None\x00")),
    ("W_None", re.compile("None".encode("utf-16-le") + b"\x00\x00")),
    ("A_Raptor", re.compile(rb"Raptor")),
    ("W_Raptor", re.compile("Raptor".encode("utf-16-le"))),
    ("A_PC", re.compile(rb"PlayerController")),
    ("W_PC", re.compile("PlayerController".encode("utf-16-le"))),
]
HIT_CAP = 10
COUNT_CAP = 100000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--max-gb", type=float, default=48.0)
    ap.add_argument("--out", default="work/names_diag.json")
    args = ap.parse_args()

    handle, world, ranges, meta = get_live_world(args.offsets)
    regions = [m for m in iter_regions(handle) if readable(m)]

    results = {name: {"count": 0, "hits": []} for name, _ in PATTERNS}
    total = 0
    SLICE = 8 * 1024 * 1024

    for mbi in regions:
        if total > args.max_gb * (1 << 30):
            break
        if all(len(results[n]["hits"]) >= HIT_CAP or results[n]["count"] >= COUNT_CAP
               for n, _ in PATTERNS):
            break
        base = mbi.BaseAddress
        size = mbi.RegionSize
        off = 0
        while off < size:
            chunk = p2.read_memory(handle, base + off, min(SLICE, size - off))
            if not chunk:
                break
            total += len(chunk)
            for name, pat in PATTERNS:
                res = results[name]
                if res["count"] >= COUNT_CAP:
                    continue
                for m in pat.finditer(chunk):
                    res["count"] += 1
                    if len(res["hits"]) >= HIT_CAP:
                        continue
                    addr = base + off + m.start()
                    before = p2.read_memory(handle, addr - 16, 16) or b""
                    after = p2.read_memory(handle, addr, 48) or b""
                    res["hits"].append({
                        "addr": hex(addr),
                        "regionType": hex(mbi.Type),
                        "before": before.hex(" "),
                        "after": after.hex(" "),
                    })
                    if res["count"] >= COUNT_CAP:
                        break
            off += len(chunk)

    out = {**meta, "scannedBytes": total, "patterns": results}
    d = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(d, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()