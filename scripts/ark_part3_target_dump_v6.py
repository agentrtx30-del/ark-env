#!/usr/bin/env python3
r"""Part 3 target dump v6: resolvable-marker creature detection. Singleplayer/local only.
Usage: python scripts/ark_part3_target_dump_v6.py --out work/targets.json --radius 60000
"""
import argparse, json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (build_name_table, resolve_name, iter_regions,
                                  readable, MEM_PRIVATE, MEM_MAPPED)

def chain(handle, idmap, cp, depth=12):
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen:
            break
        seen.add(cp)
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        out.append(resolve_name(idmap, idx) or f"C_{idx}")
        cp = p2.read_u64(handle, cp + 0x30)
    return out

def get_loc(handle, a, caps_off):
    loc = p2.read_floats(handle, a + 0xc00, 3)
    if loc and all(x is not None and abs(x) < 1e6 for x in loc):
        return loc
    c = p2.read_u64(handle, a + caps_off)
    if c:
        l2 = p2.read_floats(handle, c + 0xf0, 3)
        if l2 and all(x is not None for x in l2):
            return l2
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--out", default="work/targets.json")
    ap.add_argument("--radius", type=float, default=60000)
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
    caps_off = int(A.get("capsuleOffset", "0x268"), 16)

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    level = p2.read_u64(handle, world + int(A["actorArrayLevelOffset"], 16))
    arr = level + int(A["actorArrayOffset"], 16)
    data = p2.read_u64(handle, arr); count = p2.read_i32(handle, arr + 8) or 0

    info = []
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges):
            continue
        ch = chain(handle, idmap, p2.read_u64(handle, a + 0x10))
        info.append((a, ch, "|".join(ch)))

    pstates = [a for a, ch, j in info if ch and ch[0] == "ShooterPlayerState"]
    pset = set(pstates)
    creatures = [a for a, ch, j in info if ("Character" in j or "Pawn" in j)]
    cset = set(creatures)

    controller = own = None
    for a, ch, j in info:
        if a in pset or a in cset:
            continue
        blob = p2.read_memory(handle, a, 0x1000)
        if not blob:
            continue
        vals = set(struct.unpack(f"<{len(blob)//8}Q", blob[:len(blob)//8*8]))
        if not (vals & pset):
            continue
        pw = next((p for p in creatures if p in vals), None)
        if pw:
            controller, own = a, pw
            break

    ps_packed = struct.pack("<Q", pstates[0]) if pstates else b""
    ctrl_packed = struct.pack("<Q", controller) if controller else b""

    own_loc = get_loc(handle, own, caps_off) if own else None
    cam = own_loc or (0.0, 0.0, 0.0)

    targets = []
    for a, ch, j in info:
        if a == own:
            label = "player"
        elif "Character" in j or "Pawn" in j:
            blob = p2.read_memory(handle, a, 0x2000) or b""
            label = "tamed" if (ps_packed and ps_packed in blob) or (ctrl_packed and ctrl_packed in blob) else "wild"
        else:
            continue
        loc = get_loc(handle, a, caps_off)
        if not loc:
            continue
        dist = math.dist(loc, cam)
        if label != "player" and dist > args.radius:
            continue
        caps = p2.read_u64(handle, a + caps_off)
        hh = p2.read_floats(handle, caps + int(CH["capsuleHalfHeightOffset"], 16), 1)[0] if caps else None
        hp = p2.read_floats(handle, a + int(CH["healthOffset"], 16), 1)[0]
        mhp = p2.read_floats(handle, a + int(CH["maxHealthOffset"], 16), 1)[0]
        nidx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
        name = "YOU" if label == "player" else (resolve_name(idmap, nidx) or f"id_{nidx}")
        targets.append({"ptr": hex(a), "label": label, "class": ch[0], "name": name,
                        "loc": loc, "distance": round(dist, 1),
                        "capsuleHalfHeight": hh, "health": hp, "maxHealth": mhp})

    targets.sort(key=lambda t: t["distance"])
    counts = {}
    for t in targets:
        counts[t["label"]] = counts.get(t["label"], 0) + 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    json.dump({"camera": list(cam), "tableSize": len(idmap), "counts": counts,
               "count": len(targets),
               "structural_lock": {"ownPawn": hex(own) if own else None,
                                   "controller": hex(controller) if controller else None,
                                   "playerState": hex(pstates[0]) if pstates else None},
               "targets": targets}, open(args.out, "w"), indent=2)
    for t in targets[:40]:
        print(f"{t['distance']:>10.1f}  {t['label']:<6} {t['class']:<22} "
              f"hp={t['health']}/{t['maxHealth']} hh={t['capsuleHalfHeight']} {t['name']}")
    print("...", json.dumps(counts))

if __name__ == "__main__":
    main()