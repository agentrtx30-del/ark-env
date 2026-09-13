#!/usr/bin/env python3
r"""
ARK Part 3 — FName resolver v11 (descriptor directory + interior-pointer RVA).
Singleplayer/local diagnostic only.

Usage:
  python scripts/ark_part3_names.py find-names --offsets offsets/ark_offsets.json --out work/names.json
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3 import get_live_world

MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x00020000
MEM_MAPPED = 0x00040000
MEM_IMAGE = 0x01000000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
MAX_ADDR = 0x7FFFFFFFFFFF

IDS = [0, 1, 2, 3, 4, 5, 6, 217, 9096, 9670, 11162, 11218, 11235, 12854,
       13022, 13375, 238923, 239020, 239972, 240500, 241080, 241298, 241421]


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


kernel32 = ctypes.windll.kernel32
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


def query_region(handle, addr):
    mbi = MBI()
    if kernel32.VirtualQueryEx(handle, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
        return mbi
    return None


def read_regions(handle, base, max_bytes):
    out = bytearray()
    addr = base
    end = base + max_bytes
    while addr < end:
        mbi = query_region(handle, addr)
        if not mbi or mbi.RegionSize == 0:
            break
        r_end = mbi.BaseAddress + mbi.RegionSize
        if not readable(mbi):
            break
        seg = p2.read_memory(handle, addr, min(r_end, end) - addr)
        if not seg:
            break
        out += seg
        addr = r_end
    return bytes(out)


def scan_regions(handle, regions, pattern, limit, max_bytes):
    hits = []
    total = 0
    SLICE = 8 * 1024 * 1024
    for mbi in regions:
        if total > max_bytes or len(hits) >= limit:
            break
        base = mbi.BaseAddress
        size = mbi.RegionSize
        off = 0
        tail = b""
        while off < size and len(hits) < limit:
            chunk = p2.read_memory(handle, base + off, min(SLICE, size - off))
            if not chunk:
                break
            total += len(chunk)
            blob = tail + chunk
            start = 0
            while True:
                m = pattern.search(blob, start)
                if not m:
                    break
                hits.append(base + off - len(tail) + m.start())
                start = m.end()
                if len(hits) >= limit:
                    break
            tail = blob[-64:]
            off += len(chunk)
    return hits


def parse_entry(blob, o):
    if o + 16 > len(blob):
        return None
    idf, aux = struct.unpack_from("<QQ", blob, o)
    wide = idf & 1
    s = o + 16
    if wide:
        ln = 0
        while s + 2 * ln + 1 < len(blob):
            if blob[s + 2 * ln] == 0 and blob[s + 2 * ln + 1] == 0:
                break
            ln += 1
        else:
            return None
        if ln < 1 or ln > 1024:
            return None
        try:
            name = blob[s:s + 2 * ln].decode("utf-16-le")
        except UnicodeDecodeError:
            return None
        pad = ((2 * ln + 2) + 7) & ~7
    else:
        nul = blob.find(b"\x00", s, s + 1024)
        if nul < 0:
            return None
        raw = blob[s:nul]
        if not raw or not all(32 <= b < 127 for b in raw):
            return None
        name = raw.decode("latin-1")
        pad = ((nul - s + 1) + 7) & ~7
    return idf, name, s + pad


def walk_blob(blob, max_entries=400000):
    out = []
    o = 0
    last = -1
    while o + 16 <= len(blob) and len(out) < max_entries:
        e = parse_entry(blob, o)
        if not e or e[2] <= o or e[0] <= last:
            break
        last = e[0]
        out.append((e[0], e[1]))
        o = e[2]
    return out


def plausible_ptr(v):
    return bool(v) and not (v & 7) and 0x10000 <= v < 0x800000000000


def hunt_best(handle, private, values, maxb):
    pat = re.compile(b"|".join(re.escape(struct.pack("<Q", v)) for v in sorted(set(values))))
    best = {}
    total = 0
    SLICE = 8 * 1024 * 1024
    for mbi2 in private:
        if total > maxb:
            break
        base = mbi2.BaseAddress
        size = mbi2.RegionSize
        off = 0
        while off < size:
            chunk = p2.read_memory(handle, base + off, min(SLICE, size - off))
            if not chunk:
                break
            total += len(chunk)
            for m in pat.finditer(chunk):
                v = struct.unpack_from("<Q", chunk, m.start())[0]
                A = base + off + m.start()
                blob = p2.read_memory(handle, A, 0x800)
                if not blob:
                    continue
                e1 = parse_entry(blob, 0)
                if not e1 or e1[0] != v:
                    continue
                aux = struct.unpack_from("<Q", blob, 8)[0]
                aux_ok = (aux == 0) or plausible_ptr(aux)
                e2 = parse_entry(blob, e1[2])
                chain_ok = bool(e2) and e2[0] in (v + 1, v + 2)
                e3_ok = False
                if e2:
                    e3 = parse_entry(blob, e2[2])
                    e3_ok = bool(e3) and e3[0] in (e2[0] + 1, e2[0] + 2)
                name = e1[1]
                charset_ok = 3 <= len(name) <= 128 and all(
                    c.isalnum() or c in "._-/ " for c in name)
                score = (4 if chain_ok and e3_ok else 0) + (2 if chain_ok else 0) \
                    + (1 if aux_ok else 0) + (1 if charset_ok else 0)
                if score < 2:
                    continue
                if v not in best or score > best[v][0]:
                    best[v] = (score, name)
            off += len(chunk)
    return {v: name for v, (_, name) in best.items()}


def cmd_find_names(args):
    handle, world, ranges, meta = get_live_world(args.offsets)
    mod_base = int(meta["module_base"], 16)

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    image = [m for m in regions if m.Type == MEM_IMAGE]
    maxb = args.max_gb * (1 << 30)

    ANCHOR = re.compile(rb"None\x00\x00\x00\x00.{16}ByteProperty\x00", re.DOTALL)
    hits = scan_regions(handle, private, ANCHOR, 4, maxb)
    if not hits:
        sys.exit(json.dumps({"error": "anchor not found"}, indent=2))
    s_none = hits[0]
    mbi = query_region(handle, s_none)
    chunk0 = mbi.AllocationBase if mbi else 0

    idmap = {}
    blocks = []
    seen = set()
    dir_first = None
    if chunk0:
        blocks.append(chunk0)
        seen.add(chunk0)
        idmap.update(walk_blob(read_regions(handle, chunk0, 512 * 1024)))

        win = p2.read_memory(handle, chunk0 - args.dir_window, args.dir_window) or b""
        budget = 20000
        for i in range(0, len(win) - 8, 8):
            d = struct.unpack_from("<Q", win, i)[0]
            if not plausible_ptr(d):
                continue
            md = query_region(handle, d)
            if not md or not readable(md) or md.Type not in (MEM_PRIVATE, MEM_MAPPED):
                continue
            dblob = p2.read_memory(handle, d, 0x800)
            if not dblob:
                continue
            found_here = 0
            for o in range(0, len(dblob) - 8, 8):
                if budget <= 0:
                    break
                b = struct.unpack_from("<Q", dblob, o)[0]
                if not plausible_ptr(b):
                    continue
                budget -= 1
                mb = query_region(handle, b)
                if not mb or not readable(mb):
                    continue
                base = mb.AllocationBase
                if base in seen:
                    continue
                seen.add(base)
                entries = walk_blob(read_regions(handle, base, 256 * 1024))
                if len(entries) >= 16:
                    blocks.append(base)
                    idmap.update(entries)
                    found_here += 1
                if len(blocks) >= args.max_blocks:
                    break
            if found_here and dir_first is None:
                dir_first = chunk0 - args.dir_window + i
            if len(blocks) >= args.max_blocks or budget <= 0:
                break

    # ---- version-stable RVA: any image pointer into the pool object ----
    pool_obj = None
    m2 = query_region(handle, chunk0 - 0x60) if chunk0 else None
    if m2:
        pool_obj = m2.AllocationBase
    rva = None
    delta = None
    img_size = 0
    for (b, s, n) in ranges:
        if b == mod_base:
            img_size = s
            break
    if pool_obj and img_size:
        img = read_regions(handle, mod_base, img_size)
        if img:
            interior = set(pool_obj + off for off in range(0, args.pool_span, 8))
            nq = len(img) // 8
            vals = struct.unpack("<%dQ" % nq, img[:nq * 8])
            for idx, v in enumerate(vals):
                if v in interior:
                    rva = idx * 8
                    delta = v - pool_obj
                    break

    # ---- fallback hunt for anything still missing ----
    missing = []
    for i in IDS:
        if 2 * i not in idmap and 2 * i + 1 not in idmap:
            missing += [2 * i, 2 * i + 1]
    if missing:
        idmap.update(hunt_best(handle, private, missing, maxb))

    resolved = {str(i): idmap.get(2 * i, idmap.get(2 * i + 1)) for i in IDS}

    result = {**meta,
              "anchor": hex(s_none),
              "chunk0Base": hex(chunk0) if chunk0 else None,
              "poolObject": hex(pool_obj) if pool_obj else None,
              "dirFirst": hex(dir_first) if dir_first else None,
              "blocksWalked": len(blocks),
              "tableSize": len(idmap),
              "maxId": max(idmap) if idmap else None}
    if rva is not None:
        result["gnamesGlobal"] = hex(mod_base + rva)
        result["rva"] = hex(rva)
        result["poolObjectDelta"] = delta
        result["fnaNameTable"] = {"module": meta["module"], "rva": hex(rva)}
    result["resolved"] = resolved

    if args.out:
        d = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(d, exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))


def main():
    ap = argparse.ArgumentParser(description="FName resolver v11 (local/singleplayer only)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("find-names")
    p.add_argument("--offsets", default="offsets/ark_offsets.json")
    p.add_argument("--max-gb", type=float, default=48.0)
    p.add_argument("--dir-window", type=lambda x: int(x, 0), default=0x4000)
    p.add_argument("--max-blocks", type=int, default=400)
    p.add_argument("--pool-span", type=lambda x: int(x, 0), default=0x40000)
    p.add_argument("--out", default="work/names.json")
    p.set_defaults(func=cmd_find_names)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()