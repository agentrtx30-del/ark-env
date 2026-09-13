#!/usr/bin/env python3
r"""
ARK Part 3 — Class & Location Dump (repaired). Singleplayer/local diagnostic only.
Usage: python scripts/ark_part3_class_dump.py --offsets offsets/ark_offsets.json --out work/class_dump.json
"""
import argparse, ctypes, ctypes.wintypes as wt, json, math, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2

MEM_COMMIT, MEM_PRIVATE, MEM_MAPPED, PAGE_NOACCESS, PAGE_GUARD, MAX_ADDR = 0x1000, 0x20000, 0x40000, 0x01, 0x100, 0x7FFFFFFFFFFF

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD)]

kernel32 = ctypes.windll.kernel32
kernel32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]

def query_region(h, a):
    m = MBI()
    return m if kernel32.VirtualQueryEx(h, a, ctypes.byref(m), ctypes.sizeof(m)) else None

def readable(m): return m.State == MEM_COMMIT and not (m.Protect & PAGE_GUARD) and m.Protect not in (0, PAGE_NOACCESS)

def iter_regions(h):
    a = 0
    while a < MAX_ADDR:
        m = query_region(h, a)
        if not m or m.RegionSize == 0: break
        yield m
        a = m.BaseAddress + m.RegionSize

def read_regions(h, b, mx):
    out, a, end = bytearray(), b, b + mx
    while a < end:
        m = query_region(h, a)
        if not m or m.RegionSize == 0 or not readable(m): break
        r = m.BaseAddress + m.RegionSize
        s = p2.read_memory(h, a, min(r, end) - a)
        if not s: break
        out += s; a = r
    return bytes(out)

def scan_regions(h, regs, pat, lim, mx):
    hits, tot, SL = [], 0, 8 * 1024 * 1024
    for m in regs:
        if tot > mx or len(hits) >= lim: break
        b, sz, off, tail = m.BaseAddress, m.RegionSize, 0, b""
        while off < sz and len(hits) < lim:
            c = p2.read_memory(h, b + off, min(SL, sz - off))
            if not c: break
            tot += len(c); blob = tail + c; st = 0
            while True:
                mt = pat.search(blob, st)
                if not mt: break
                hits.append(b + off - len(tail) + mt.start()); st = mt.end()
                if len(hits) >= lim: break
            tail = blob[-64:]; off += len(c)
    return hits

def parse_entry(blob, o):
    if o + 16 > len(blob): return None
    idf, aux = struct.unpack_from("<QQ", blob, o)
    wide = idf & 1; s = o + 16
    if wide:
        ln = 0
        while s + 2 * ln + 1 < len(blob):
            if blob[s + 2 * ln] == 0 and blob[s + 2 * ln + 1] == 0: break
            ln += 1
        else: return None
        if ln < 1 or ln > 1024: return None
        try: name = blob[s:s + 2 * ln].decode("utf-16-le")
        except UnicodeDecodeError: return None
        pad = ((2 * ln + 2) + 7) & ~7
    else:
        nul = blob.find(b"\x00", s, s + 1024)
        if nul < 0: return None
        raw = blob[s:nul]
        if not raw or not all(32 <= b < 127 for b in raw): return None
        name = raw.decode("latin-1"); pad = ((nul - s + 1) + 7) & ~7
    return idf, name, s + pad

def walk_blob(blob, mx=400000):
    out, o, last = [], 0, -1
    while o + 16 <= len(blob) and len(out) < mx:
        e = parse_entry(blob, o)
        if not e or e[2] <= o or e[0] <= last: break
        last = e[0]; out.append((e[0], e[1])); o = e[2]
    return out

def plausible_ptr(v): return bool(v) and not (v & 7) and 0x10000 <= v < 0x800000000000

def build_name_table(handle, regions, private, maxb):
    ANCHOR = re.compile(rb"None\x00\x00\x00\x00.{16}ByteProperty\x00", re.DOTALL)
    hits = scan_regions(handle, private, ANCHOR, 4, maxb)
    if not hits: return {}
    mbi = query_region(handle, hits[0])
    chunk0 = mbi.AllocationBase if mbi else 0
    idmap, seen = {}, set()
    if chunk0:
        seen.add(chunk0); idmap.update(walk_blob(read_regions(handle, chunk0, 512 * 1024)))
        win = p2.read_memory(handle, chunk0 - 0x4000, 0x4000) or b""
        budget = 20000
        for i in range(0, len(win) - 8, 8):
            d = struct.unpack_from("<Q", win, i)[0]
            if not plausible_ptr(d): continue
            md = query_region(handle, d)
            if not md or not readable(md): continue
            dblob = p2.read_memory(handle, d, 0x800)
            if not dblob: continue
            for o in range(0, len(dblob) - 8, 8):
                if budget <= 0: break
                b = struct.unpack_from("<Q", dblob, o)[0]
                if not plausible_ptr(b): continue
                budget -= 1
                mb = query_region(handle, b)
                if not mb or not readable(mb): continue
                base = mb.AllocationBase
                if base in seen: continue
                seen.add(base)
                entries = walk_blob(read_regions(handle, base, 256 * 1024))
                if len(entries) >= 16: idmap.update(entries)
                if len(idmap) > 250000: break
            if len(idmap) > 250000 or budget <= 0: break
    return idmap

def resolve_name(idmap, idx):
    return idmap.get(2 * idx) or idmap.get(2 * idx + 1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--max-gb", type=float, default=48.0)
    ap.add_argument("--out", default="work/class_dump.json")
    args = ap.parse_args()

    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    if not handle: sys.exit("OpenProcess failed. Run elevated.")
    offsets = p2.load_offsets(args.offsets)

    actor = offsets.get("actor") or {}
    if not actor.get("actorArrayLevelOffset") or not actor.get("actorArrayOffset"):
        sys.exit("offsets/ark_offsets.json is missing the actor section - replace it with the repaired version first.")

    mod, modules = p2.get_target_module(handle, offsets.get("engineModule", "ShooterGame.exe"))
    base = mod["base"]
    module_ranges = [(m["base"], m["size"], m["name"]) for m in modules]

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    level = p2.read_u64(handle, world + int(actor["actorArrayLevelOffset"], 16))
    actors_array = level + int(actor["actorArrayOffset"], 16)
    data_ptr = p2.read_u64(handle, actors_array)
    count = p2.read_i32(handle, actors_array + 8)

    class_counts, samples, class_ptrs = {}, [], []
    for i in range(min(count or 0, 3000)):
        a = p2.read_u64(handle, data_ptr + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, module_ranges): continue

        n_idx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
        actor_name = resolve_name(idmap, n_idx) or f"Unknown_{n_idx}"

        class_ptr = p2.read_u64(handle, a + 0x10)
        class_name = "Unknown"
        if class_ptr and p2.is_valid_ptr(handle, class_ptr, module_ranges):
            c_idx = (p2.read_u64(handle, class_ptr + 0x18) or 0) & 0xFFFFFFFF
            class_name = resolve_name(idmap, c_idx) or f"Class_{c_idx}"
            if len(class_ptrs) < 8 and class_ptr not in class_ptrs:
                class_ptrs.append(class_ptr)

        class_counts[class_name] = class_counts.get(class_name, 0) + 1

        if len(samples) < 60:
            root_comp = None
            for off in range(0x100, 0x300, 8):
                ptr = p2.read_u64(handle, a + off)
                if ptr and p2.is_valid_ptr(handle, ptr, module_ranges):
                    ci = (p2.read_u64(handle, ptr + 0x18) or 0) & 0xFFFFFFFF
                    cn = resolve_name(idmap, ci)
                    if cn and "Component" in cn:
                        root_comp = ptr; break
            loc = None
            if root_comp:
                for off in range(0x100, 0x200, 4):
                    f1 = p2.read_floats(handle, root_comp + off, 3)
                    if f1 and all(f is not None for f in f1):
                        if all(abs(f) < 800000 for f in f1) and any(f != 0 for f in f1):
                            loc = f1; break
            samples.append({"i": i, "actor": hex(a), "name": actor_name, "class": class_name,
                            "root": hex(root_comp) if root_comp else None, "loc": loc})

    # superclass-offset probe (UStruct::SuperStruct candidate scan)
    super_probe = []
    for off in range(0x30, 0xA0, 8):
        ok, chains = 0, {}
        for cp in class_ptrs:
            sp = p2.read_u64(handle, cp + off)
            if not sp or sp == cp or not p2.is_valid_ptr(handle, sp, module_ranges): continue
            si = (p2.read_u64(handle, sp + 0x18) or 0) & 0xFFFFFFFF
            sn = resolve_name(idmap, si)
            if not sn: continue
            ok += 1
            if len(chains) < 6:
                ci = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
                chains[hex(cp)] = [resolve_name(idmap, ci), sn]
        if ok >= 3:
            super_probe.append({"offset": hex(off), "ok": ok, "chains": chains})

    out = {"tableSize": len(idmap), "actorCount": count, "classCounts": class_counts,
           "superProbe": super_probe, "samples": samples}
    d = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(d, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f: json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()