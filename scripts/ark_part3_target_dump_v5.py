#!/usr/bin/env python3
r"""Part 3 target dump v5: chain-based creature detection. Singleplayer/local only.
Usage: python scripts/ark_part3_target_dump_v5.py --out work/targets.json --radius 60000
"""
import argparse, json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (build_name_table, resolve_name, iter_regions,
                                  readable, MEM_PRIVATE, MEM_MAPPED)

def chain(handle, idmap, cp, depth=10):
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen: break
        seen.add(cp)
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        out.append(resolve_name(idmap, idx) or f"C_{idx}")
        cp = p2.read_u64(handle, cp + 0x30)
    return out

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
        if not a or not p2.is_valid_ptr(handle, a, ranges): continue
        cp = p2.read_u64(handle, a + 0x10)
        ch = chain(handle, idmap, cp) if cp else []
        info.append((a, ch, "|".join(ch)))

    ps = next((a for a, ch, j in info if ch and ch[0] == "ShooterPlayerState"), None)
    ps_packed = struct.pack("<Q", ps) if ps else b""
    creatures = [(a, ch, j) for a, ch, j in info if "Character" in j or "Pawn" in j]
    cset = set(a for a, _, _ in creatures)

    controller = own_pawn = None
    if ps:
        for a, ch, j in info:
            if a == ps or a in cset: continue
            blob = p2.read_memory(handle, a, 0x1000)
            if not blob or ps_packed not in blob: continue
            for c, _, _ in creatures:
                if struct.pack("<Q", c) in blob:
                    controller, own_pawn = a, c
                    break
            if controller: break
    ctrl_packed = struct.pack("<Q", controller) if controller else b""

    # camera: matrix last row if sane & near own pawn, else own pawn loc
    cam = None
    own_loc = None
    if own_pawn:
        own_loc = p2.read_floats(handle, own_pawn + int(CH["locationOffset"], 16), 3)
    vc_chain_ok = True
    try:
        engine = p2.read_u64(handle, base + int(offsets["engineGlobal"]["rva"], 16))
        gi = p2.read_u64(handle, engine + int(offsets["gameInstanceOffset"], 16))
        lp = p2.read_u64(handle, p2.read_u64(handle, gi + int(offsets["localPlayersOffset"], 16)))
        vc = p2.read_u64(handle, lp + int(offsets["viewportClientOffset"], 16))
        m16 = p2.read_floats(handle, vc + int(offsets["viewMatrixOffset"], 16), 16)
        if m16 and all(x is not None for x in m16[12:15]):
            mloc = (m16[12], m16[13], m16[14])
            if own_loc and math.dist(mloc, own_loc) < 20000:
                cam = mloc
    except Exception:
        vc_chain_ok = False
    if cam is None:
        cam = tuple(own_loc) if own_loc else (0.0, 0.0, 0.0)

    targets = []
    for a, ch, j in creatures:
        loc = p2.read_floats(handle, a + int(CH["locationOffset"], 16), 3)
        if not loc or any(x is None for x in loc): continue
        dist = math.dist(loc, cam)
        if a == own_pawn:
            label = "player"
        else:
            blob = p2.read_memory(handle, a, 0x2000) or b""
            label = "tamed" if (ps_packed and ps_packed in blob) or (ctrl_packed and ctrl_packed in blob) else "wild"
            if dist > args.radius: continue
        caps = p2.read_u64(handle, a + int(A["capsuleOffset"], 16))
        hh = rad = None
        if caps and p2.is_valid_ptr(handle, caps, ranges):
            hh = p2.read_floats(handle, caps + int(CH["capsuleHalfHeightOffset"], 16), 1)[0]
            rad = p2.read_floats(handle, caps + int(CH["capsuleRadiusOffset"], 16), 1)[0]
        nidx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
        targets.append({
            "ptr": hex(a), "label": label, "class": ch[0],
            "name": resolve_name(idmap, nidx) or f"id_{nidx}",
            "loc": loc, "distance": round(dist, 1),
            "capsuleHalfHeight": hh, "capsuleRadius": rad,
            "health": p2.read_floats(handle, a + int(CH["healthOffset"], 16), 1)[0],
            "maxHealth": p2.read_floats(handle, a + int(CH["maxHealthOffset"], 16), 1)[0],
        })

    targets.sort(key=lambda t: t["distance"])
    counts = {}
    for t in targets: counts[t["label"]] = counts.get(t["label"], 0) + 1
    out = {"camera": list(cam), "cameraSource": "matrix" if (cam != tuple(own_loc or (0,0,0))) else "pawn-fallback",
           "tableSize": len(idmap), "counts": counts, "count": len(targets),
           "structural_lock": {"ownPawn": hex(own_pawn) if own_pawn else None,
                               "controller": hex(controller) if controller else None,
                               "playerState": hex(ps) if ps else None},
           "targets": targets}
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    json.dump(out, open(args.out, "w"), indent=2)
    for t in targets[:40]:
        print(f"{t['distance']:>10.1f}  {t['label']:<6} {t['class']:<22} "
              f"hp={t['health']}/{t['maxHealth']} hh={t['capsuleHalfHeight']} {t['name']}")
    print("...", json.dumps(counts))

if __name__ == "__main__":
    main()