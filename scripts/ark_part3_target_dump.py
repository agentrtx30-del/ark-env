#!/usr/bin/env python3
r"""Part 3 final target dump v2. Singleplayer/local only.
Usage:
  python scripts/ark_part3_target_dump.py --out work/targets.json --radius 60000 [--name SurvivorName]
"""
import argparse, json, math, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2

MEM_COMMIT, MEM_PRIVATE, MEM_MAPPED = 0x1000, 0x20000, 0x40000

def plausible(v): return 0x10000 <= v < 0x800000000000 and not (v & 7)

def scan_regions(handle, regions, pattern, limit, max_bytes):
    hits, total, SL = [], 0, 8*1024*1024
    for mbi in regions:
        if total > max_bytes or len(hits) >= limit: break
        base, size, off, tail = mbi.BaseAddress, mbi.RegionSize, 0, b""
        while off < size and len(hits) < limit:
            chunk = p2.read_memory(handle, base+off, min(SL, size-off))
            if not chunk: break
            total += len(chunk); blob = tail+chunk; st = 0
            while True:
                m = pattern.search(blob, st)
                if not m: break
                hits.append(base+off-len(tail)+m.start()); st = m.end()
                if len(hits) >= limit: break
            tail = blob[-64:]; off += len(chunk)
    return hits

def parse_entry(blob, o):
    if o+16 > len(blob): return None
    idf, aux = struct.unpack_from("<QQ", blob, o)
    wide = idf & 1; s = o+16
    if wide:
        ln = 0
        while s+2*ln+1 < len(blob):
            if blob[s+2*ln] == 0 and blob[s+2*ln+1] == 0: break
            ln += 1
        else: return None
        if ln < 1 or ln > 1024: return None
        try: name = blob[s:s+2*ln].decode("utf-16-le")
        except UnicodeDecodeError: return None
        pad = ((2*ln+2)+7) & ~7
    else:
        nul = blob.find(b"\x00", s, s+1024)
        if nul < 0: return None
        raw = blob[s:nul]
        if not raw or not all(32 <= b < 127 for b in raw): return None
        name = raw.decode("latin-1"); pad = ((nul-s+1)+7) & ~7
    return idf, name, s+pad

def build_name_table(handle, private, maxb):
    ANCHOR = re.compile(rb"None\x00\x00\x00\x00.{16}ByteProperty\x00", re.DOTALL)
    hits = scan_regions(handle, private, ANCHOR, 4, maxb)
    if not hits: return {}
    mbi = p2.query_region(handle, hits[0])
    if not mbi: return {}
    chunk0 = mbi.AllocationBase
    idmap, seen = {}, {chunk0}
    def walk(base, size):
        data = p2.read_memory(handle, base, size)
        if not data: return
        o, last = 0, -1
        while o+16 <= len(data):
            e = parse_entry(data, o)
            if not e or e[2] <= o or e[0] <= last: break
            last = e[0]; idmap.setdefault(e[0], e[1]); o = e[2]
    walk(chunk0, 0x200000)
    win = p2.read_memory(handle, chunk0-0x4000, 0x4000) or b""
    descs = []
    for i in range(0, len(win)-8, 8):
        v = struct.unpack_from("<Q", win, i)[0]
        if not plausible(v): continue
        mm = p2.query_region(handle, v)
        if not mm or mm.State != MEM_COMMIT or mm.AllocationBase in seen: continue
        seen.add(mm.AllocationBase); descs.append(v); walk(mm.AllocationBase, 0x100000)
    for d in descs:
        blob = p2.read_memory(handle, d, 0x800)
        if not blob: continue
        for i in range(0, len(blob)-8, 8):
            v = struct.unpack_from("<Q", blob, i)[0]
            if not plausible(v): continue
            mm = p2.query_region(handle, v)
            if not mm or mm.State != MEM_COMMIT or mm.AllocationBase in seen: continue
            seen.add(mm.AllocationBase); walk(mm.AllocationBase, 0x100000)
    return idmap

def resolve(idmap, n): return idmap.get(2*n) or idmap.get(2*n+1)

def chain(handle, idmap, cp, depth=10):
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen: break
        seen.add(cp)
        out.append(resolve(idmap, (p2.read_u64(handle, cp+0x18) or 0) & 0xFFFFFFFF) or
                   f"C_{(p2.read_u64(handle, cp+0x18) or 0) & 0xFFFFFFFF}")
        cp = p2.read_u64(handle, cp+0x30)
    return out

def fstrings(blob):
    out = []
    for off in range(0, len(blob)-16, 8):
        ptr, ln, cap = struct.unpack_from("<Qii", blob, off)
        if not (0 < ln <= 48) or not cap or cap < ln or not plausible(ptr): continue
        out.append((off, ptr, ln))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--out", default="work/targets.json")
    ap.add_argument("--radius", type=float, default=60000)
    ap.add_argument("--name", default=None)
    ap.add_argument("--max-gb", type=float, default=48.0)
    args = ap.parse_args()

    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(args.offsets)
    mod, modules = p2.get_target_module(handle, offsets["engineModule"])
    base = mod["base"]
    ranges = [(m["base"], m["size"], m["name"]) for m in modules]
    A = offsets["actor"]; CH = A["character"]

    regions = [m for m in p2.__dict__.get("iter_regions", lambda h: [])(handle)] or []
    if not regions:
        import ctypes, ctypes.wintypes as wt
        k32 = ctypes.windll.kernel32
        class MBI(ctypes.Structure):
            _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                        ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                        ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD),
                        ("Protect", wt.DWORD), ("Type", wt.DWORD)]
        k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
        a = 0
        while a < 0x7FFFFFFFFFFF:
            m = MBI()
            if not k32.VirtualQueryEx(handle, a, ctypes.byref(m), ctypes.sizeof(m)) or m.RegionSize == 0: break
            if m.State == MEM_COMMIT and not (m.Protect & 0x100) and m.Protect not in (0, 1):
                regions.append(m)
            a = m.BaseAddress + m.RegionSize
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]

    idmap = build_name_table(handle, private, args.max_gb*(1 << 30))

    engine = p2.read_u64(handle, base + int(offsets["engineGlobal"]["rva"], 16))
    gi = p2.read_u64(handle, engine + int(offsets["gameInstanceOffset"], 16))
    lp = p2.read_u64(handle, p2.read_u64(handle, gi + int(offsets["localPlayersOffset"], 16)))
    vc = p2.read_u64(handle, lp + int(offsets["viewportClientOffset"], 16))
    m16 = p2.read_floats(handle, vc + int(offsets["viewMatrixOffset"], 16), 16)
    cam = (m16[12], m16[13], m16[14]) if m16 and m16[12] is not None else (0, 0, 0)

    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    level = p2.read_u64(handle, world + int(A["actorArrayLevelOffset"], 16))
    arr = level + int(A["actorArrayOffset"], 16)
    data = p2.read_u64(handle, arr); count = p2.read_i32(handle, arr+8) or 0

    actors = []
    for i in range(count):
        a = p2.read_u64(handle, data + i*8)
        if not a or not p2.is_valid_ptr(handle, a, ranges): continue
        ch = chain(handle, idmap, p2.read_u64(handle, a+0x10))
        loc = None
        for coff in (int(A["capsuleOffset"], 16), int(A.get("rootComponentOffset", "0x170"), 16)):
            c = p2.read_u64(handle, a+coff)
            if c and p2.is_valid_ptr(handle, c, ranges):
                f = p2.read_floats(handle, c + int(CH["componentLocationOffset"], 16), 3)
                if f and all(x is not None for x in f): loc = f; break
        actors.append([a, ch, loc])

    pstates = [a for a, ch, _ in actors if ch and ch[0] == "ShooterPlayerState"]
    ps_set = set(pstates)
    pawn_ptrs = {a for a, ch, _ in actors if any("Character" in n or "Pawn" in n for n in ch)}
    ctrl_packed = []
    player_pawns = set()
    controllers = []
    for a, ch, _ in actors:
        if a in ps_set or a in pawn_ptrs: continue
        blob = p2.read_memory(handle, a, 0x1000)
        if not blob: continue
        vals = struct.unpack(f"<{len(blob)//8}Q", blob[:len(blob)//8*8])
        has_ps = any(v in ps_set for v in vals)
        pw = next((v for v in vals if v in pawn_ptrs), None)
        if has_ps and pw:
            controllers.append(a); player_pawns.add(pw); ctrl_packed.append(struct.pack("<Q", a))
    ps_packed = [struct.pack("<Q", p) for p in pstates]

    name_off = None
    if args.name and pstates:
        want = args.name.encode("utf-16-le")
        for ps in pstates:
            blob = p2.read_memory(handle, ps, 0x8000) or b""
            for off, ptr, ln in fstrings(blob):
                s = p2.read_memory(handle, ptr, ln*2)
                if s and s[:len(want)] == want:
                    name_off = hex(off); break
            if name_off: break

    dino_mk = A["classification"]["dinoMarkers"]
    targets = []
    for a, ch, loc in actors:
        joined = "|".join(ch)
        if a in player_pawns: label = "player"
        elif any(mk in joined for mk in dino_mk):
            blob = p2.read_memory(handle, a, 0x2000) or b""
            label = "tamed" if any(p in blob for p in ps_packed+ctrl_packed) else "wild"
        else:
            continue
        if not loc: continue
        dist = math.dist(loc, cam)
        if dist > args.radius and label != "player": continue
        caps = p2.read_u64(handle, a + int(A["capsuleOffset"], 16))
        hh = p2.read_floats(handle, caps + int(CH["capsuleHalfHeightOffset"], 16), 1)[0] if caps else None
        rad = p2.read_floats(handle, caps + int(CH["capsuleRadiusOffset"], 16), 1)[0] if caps else None
        t = {"ptr": hex(a), "label": label, "class": ch[0], "chain": ch[:4],
             "name": resolve(idmap, (p2.read_u64(handle, a+0x18) or 0) & 0xFFFFFFFF) or
                     f"id_{(p2.read_u64(handle, a+0x18) or 0) & 0xFFFFFFFF}",
             "loc": loc, "distance": round(dist, 1),
             "capsuleHalfHeight": hh, "capsuleRadius": rad,
             "health": p2.read_floats(handle, a + int(CH["healthOffset"], 16), 1)[0],
             "maxHealth": p2.read_floats(handle, a + int(CH["maxHealthOffset"], 16), 1)[0]}
        if label == "player" and name_off:
            ps = next((p for p in pstates), None)
            ptr = p2.read_u64(handle, ps + int(name_off, 16))
            ln = p2.read_i32(handle, ps + int(name_off, 16) + 8)
            s = p2.read_memory(handle, ptr, ln*2)
            t["playerName"] = s.decode("utf-16-le", "replace") if s else None
        targets.append(t)
    targets.sort(key=lambda t: t["distance"])

    out = {"camera": list(cam), "tableSize": len(idmap), "nameOffsetCandidate": name_off,
           "count": len(targets), "targets": targets}
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    json.dump(out, open(args.out, "w"), indent=2)
    for t in targets[:40]:
        print(f"{t['distance']:>10.1f}  {t['label']:<6} {t['class']:<22} "
              f"hp={t['health']}/{t['maxHealth']} hh={t['capsuleHalfHeight']} {t['name']}")
    print("...")

if __name__ == "__main__":
    main()