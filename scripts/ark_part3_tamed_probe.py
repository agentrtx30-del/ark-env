#!/usr/bin/env python3
r"""Tamed-vs-wild discovery probe.
Finds one KNOWN tamed dino by level, diffs it against wild dinos,
and reports ownership markers:
  - owner PlayerState/Controller pointers anywhere in the pawn blob
  - int/bool fields that differ (TamingTeamID / bIsTamed style)
on the pawn and on its status component.

Usage (stand next to the tamed dino, same session):
python scripts/ark_part3_tamed_probe.py --tamed-level 225 --out work/tamed_probe.json
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (build_name_table, resolve_name, iter_regions,
                                  readable, MEM_PRIVATE, MEM_MAPPED)

def pint(v):
    if v is None: return None
    if isinstance(v, int): return v
    s = str(v).strip()
    return int(s, 0) if s else None

def read_blob_chunked(handle, addr, size, chunk=0x1000):
    out = bytearray(); off = 0
    while off < size:
        n = min(chunk, size - off)
        part = p2.read_memory(handle, addr + off, n)
        if not part: break
        out += part; off += len(part)
        if len(part) < n: break
    return bytes(out)

def chain(handle, idmap, cp, depth=12, super_off=0x30):
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen: break
        seen.add(cp)
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        out.append(resolve_name(idmap, idx) or f"id_{idx}")
        cp = p2.read_u64(handle, cp + super_off)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--tamed-level", type=int, required=True)
    ap.add_argument("--wild-count", type=int, default=4)
    ap.add_argument("--pawn-size", type=lambda x: int(x, 0), default=0x8000)
    ap.add_argument("--status-size", type=lambda x: int(x, 0), default=0x1000)
    ap.add_argument("--out", default="work/tamed_probe.json")
    args = ap.parse_args()

    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(args.offsets)
    mod, modules = p2.get_target_module(handle, offsets.get("engineModule", "ShooterGame.exe"))
    base = mod["base"]
    ranges = [(m["base"], m["size"], m["name"]) for m in modules]

    A = offsets.get("actor", {}); CH = A.get("character", {})
    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, 48 * (1 << 30))

    world = p2.read_u64(handle, base + pint(offsets["gworld"]["rva"]))
    level_obj = p2.read_u64(handle, world + pint(A["actorArrayLevelOffset"]))
    arr = level_obj + pint(A["actorArrayOffset"])
    data = p2.read_u64(handle, arr); count = p2.read_i32(handle, arr + 8) or 0
    class_off = pint(A.get("uobjectHeader", {}).get("classOffset", "0x10")) or 0x10
    super_off = pint(A.get("classSuperOffset", "0x30")) or 0x30
    status_off = pint(CH.get("statusComponentOffset", A.get("statusComponentOffset")))
    level_off = pint(CH.get("levelOffset", A.get("levelOffset")))

    pstates, controllers, creatures = [], [], []
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges): continue
        j = "|".join(chain(handle, idmap, p2.read_u64(handle, a + class_off), super_off=super_off))
        if "PlayerState" in j: pstates.append(a)
        elif "PlayerController" in j: controllers.append(a)
        elif ("Character" in j or "Pawn" in j): creatures.append(a)

    def get_level(a):
        if status_off is None or level_off is None: return None
        sc = p2.read_u64(handle, a + status_off)
        if not sc: return None
        return p2.read_i32(handle, sc + level_off)

    tamed = None
    for a in creatures:
        if get_level(a) == args.tamed_level:
            tamed = a; break
    if not tamed:
        sys.exit(f"no creature with level {args.tamed_level} found - stand near it")

    wilds = [a for a in creatures if a != tamed][:args.wild_count]
    tamed_blob = read_blob_chunked(handle, tamed, args.pawn_size)
    wild_blobs = [read_blob_chunked(handle, w, args.pawn_size) for w in wilds]

    # 1) owner pointer scan inside the tamed pawn blob
    ptr_index = {}
    for ps in pstates: ptr_index[ps] = "playerstate"
    for c in controllers: ptr_index[c] = "controller"
    owner_candidates = []
    for off in range(0, len(tamed_blob) - 8, 8):
        v = struct.unpack_from("<Q", tamed_blob, off)[0]
        if v in ptr_index:
            owner_candidates.append({"offset": hex(off), "kind": ptr_index[v], "points_to": hex(v)})

    # 2) int/bool diff scan
    def diff_candidates(bt, bws):
        out = []
        m = min(len(bt), min([len(b) for b in bws] or [0])) - 4
        for off in range(0, m, 4):
            tv = struct.unpack_from("<i", bt, off)[0]
            wvs = [struct.unpack_from("<i", wb, off)[0] for wb in bws]
            if tv == 1 and all(w == 0 for w in wvs):
                out.append({"kind": "bool_tamed", "offset": hex(off), "tamed": tv, "wild": wvs})
            elif 1 < tv < 0x7FFF and all(w in (0, -1) for w in wvs):
                out.append({"kind": "team_id", "offset": hex(off), "tamed": tv, "wild": wvs})
            elif tv > 0 and len(set(wvs)) == 1 and tv != wvs[0]:
                out.append({"kind": "int_diff", "offset": hex(off), "tamed": tv, "wild": wvs})
        return out[:60]

    pawn_candidates = diff_candidates(tamed_blob, wild_blobs) if wild_blobs else []

    status_candidates = []
    if status_off is not None and wild_blobs:
        sc_t = p2.read_u64(handle, tamed + status_off)
        sc_w = [p2.read_u64(handle, w + status_off) for w in wilds]
        if sc_t and all(sc_w):
            bt = read_blob_chunked(handle, sc_t, args.status_size)
            bw = [read_blob_chunked(handle, s, args.status_size) for s in sc_w]
            if bt and all(bw):
                status_candidates = diff_candidates(bt, bw)

    out = {
        "tamedPtr": hex(tamed),
        "wildPtrs": [hex(w) for w in wilds],
        "ownerPointerCandidates": owner_candidates[:20],
        "pawnCandidates": pawn_candidates,
        "statusCandidates": status_candidates,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    json.dump(out, open(args.out, "w"), indent=2)
    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()