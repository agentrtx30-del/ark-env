#!/usr/bin/env python3
r"""Part 3 probe4: structural controller/pawn discovery. Singleplayer/local only.
Usage:
  python scripts/ark_part3_probe4.py full --out work/probe4.json
  python scripts/ark_part3_probe4.py snap --out work/hp_snap.json
  (take damage in game)
  python scripts/ark_part3_probe4.py diff --snap work/hp_snap.json
"""
import argparse, json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (build_name_table, resolve_name, iter_regions,
                                  readable, MEM_PRIVATE, MEM_MAPPED)


def get_live(offsets_path):
    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(offsets_path)
    mod, modules = p2.get_target_module(handle, offsets.get("engineModule", "ShooterGame.exe"))
    return handle, mod["base"], offsets, [(m["base"], m["size"], m["name"]) for m in modules]


def chain(handle, idmap, cp, depth=10):
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen:
            break
        seen.add(cp)
        out.append(resolve_name(idmap, (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF)
                   or f"C_{(p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF}")
        cp = p2.read_u64(handle, cp + 0x30)
    return out


def fstrings(handle, obj, lo=0, hi=0x400):
    out = []
    for off in range(lo, hi, 8):
        ptr = p2.read_u64(handle, obj + off)
        ln = p2.read_i32(handle, obj + off + 8)
        cap = p2.read_i32(handle, obj + off + 12)
        if not (0 < ln <= 48) or not cap or cap < ln or not ptr or ptr & 7:
            continue
        if not p2.query_region(handle, ptr):
            continue
        raw = p2.read_memory(handle, ptr, ln * 2)
        if not raw:
            continue
        try:
            s = raw.decode("utf-16-le")
        except Exception:
            continue
        if all(32 <= ord(c) < 127 for c in s):
            out.append({"offset": hex(off), "value": s})
    return out


def cmd_full(args):
    handle, base, offsets, ranges = get_live(args.offsets)
    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    level = p2.read_u64(handle, world + int(offsets["actor"]["actorArrayLevelOffset"], 16))
    arr = level + int(offsets["actor"]["actorArrayOffset"], 16)
    data = p2.read_u64(handle, arr)
    count = p2.read_i32(handle, arr + 8) or 0

    actors = []
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges):
            continue
        ch = chain(handle, idmap, p2.read_u64(handle, a + 0x10))
        loc = None
        for coff in (0x268, 0x170):
            comp = p2.read_u64(handle, a + coff)
            if comp and p2.is_valid_ptr(handle, comp, ranges):
                f = p2.read_floats(handle, comp + 0xf0, 3)
                if f and all(x is not None for x in f):
                    loc = f
                    break
        actors.append((a, ch, loc))

    ps = next((a for a, ch, _ in actors if ch and ch[0] == "ShooterPlayerState"), None)

    engine = p2.read_u64(handle, base + int(offsets["engineGlobal"]["rva"], 16))
    gi = p2.read_u64(handle, engine + int(offsets["gameInstanceOffset"], 16))
    lp = p2.read_u64(handle, p2.read_u64(handle, gi + int(offsets["localPlayersOffset"], 16)))
    vc = p2.read_u64(handle, lp + int(offsets["viewportClientOffset"], 16))
    m = p2.read_floats(handle, vc + int(offsets["viewMatrixOffset"], 16), 16)
    cam = [m[12], m[13], m[14]] if m and m[12] is not None else None

    own_pawn, bestd = None, 1e18
    if cam:
        for a, ch, loc in actors:
            if not loc or "ShooterCharacter" not in ch:
                continue
            d = math.dist(loc, cam)
            if d < bestd:
                bestd, own_pawn = d, a
        if bestd > 4000:
            own_pawn = None

    controller = cps_off = cp_off = None
    if ps and own_pawn:
        pps, ppw = struct.pack("<Q", ps), struct.pack("<Q", own_pawn)
        for a, ch, _ in actors:
            if a in (ps, own_pawn):
                continue
            blob = p2.read_memory(handle, a, 0x800)
            if not blob:
                continue
            i1, i2 = blob.find(pps), blob.find(ppw)
            if i1 >= 0 and i2 >= 0 and i1 % 8 == 0 and i2 % 8 == 0:
                controller, cps_off, cp_off = a, hex(i1), hex(i2)
                break

    lpco = None
    if controller and lp:
        lpb = p2.read_memory(handle, lp, 0x2000)
        if lpb:
            j = lpb.find(struct.pack("<Q", controller))
            if j >= 0 and j % 8 == 0:
                lpco = hex(j)

    caps = {}
    others = [a for a, ch, _ in actors if a != own_pawn and ("Character" in ch or "Pawn" in ch)][:3]
    for label, obj in [("own", own_pawn)] + [(f"other{k}", o) for k, o in enumerate(others)]:
        if not obj:
            continue
        c = p2.read_u64(handle, obj + 0x268)
        if c:
            caps[label] = {"capsule": hex(c), "floats_0x110": p2.read_floats(handle, c + 0x110, 12)}

    out = {"tableSize": len(idmap), "camera": cam, "playerState": hex(ps) if ps else None,
           "ownPawn": hex(own_pawn) if own_pawn else None, "pawnCameraDist": bestd,
           "controller": hex(controller) if controller else None,
           "controllerPlayerStateOffset": cps_off, "controllerPawnOffset": cp_off,
           "localPlayerControllerOffset": lpco,
           "playerStateStrings": fstrings(handle, ps) if ps else [],
           "capsuleCheck": caps}

    d = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(d, exist_ok=True)
    if own_pawn:
        with open(os.path.join(d, "pawn_info.json"), "w") as f:
            json.dump({"pawn": hex(own_pawn)}, f)
    with open(args.out, "w") as f:
        json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))


def cmd_snap(args):
    handle, base, offsets, ranges = get_live(args.offsets)
    d = os.path.dirname(os.path.abspath(args.out))
    ip = os.path.join(d, "pawn_info.json")
    if not os.path.isfile(ip):
        sys.exit("pawn_info.json missing - run 'full' first.")
    pawn = int(json.load(open(ip))["pawn"], 16)
    vals = {hex(o): p2.read_floats(handle, pawn + o, 1)[0] for o in range(0x0, 0x1000, 4)}
    json.dump({"pawn": hex(pawn), "floats": vals}, open(args.out, "w"), indent=2)
    print("snapshot saved:", args.out)


def cmd_diff(args):
    handle, base, offsets, ranges = get_live(args.offsets)
    if not os.path.isfile(args.snap):
        sys.exit("snapshot missing - run 'snap' first.")
    snap = json.load(open(args.snap))
    pawn = int(snap["pawn"], 16)
    changed = []
    for o in range(0x0, 0x1000, 4):
        old = snap["floats"].get(hex(o))
        new = p2.read_floats(handle, pawn + o, 1)[0]
        if old is not None and new is not None and 0.01 < abs(old - new) < 1000:
            changed.append({"offset": hex(o), "before": old, "after": new})
    print(json.dumps({"changed": changed}, indent=2))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("full", cmd_full), ("snap", cmd_snap), ("diff", cmd_diff)):
        p = sub.add_parser(name)
        p.add_argument("--offsets", default="offsets/ark_offsets.json")
        p.add_argument("--max-gb", type=float, default=48.0)
        p.add_argument("--out", default="work/probe4.json")
        p.add_argument("--snap", default="work/hp_snap.json")
        p.set_defaults(func=fn)
    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()