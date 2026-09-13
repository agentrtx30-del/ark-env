#!/usr/bin/env python3
r"""Part 3 final probe (singleplayer/local only).
Usage:
  python scripts/ark_part3_probe2.py full  --offsets offsets/ark_offsets.json --out work/probe2.json
  python scripts/ark_part3_probe2.py snap  --offsets offsets/ark_offsets.json --out work/hp_snap.json
  ...take damage in game (punch a wall / fall)...
  python scripts/ark_part3_probe2.py diff  --offsets offsets/ark_offsets.json --snap work/hp_snap.json
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (iter_regions, readable, read_regions, parse_entry,
                                  walk_blob, query_region)

def resolve(idmap, n):
    return idmap.get(2 * n) or idmap.get(2 * n + 1)

def chain(handle, idmap, cp, depth=10):
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen: break
        seen.add(cp)
        out.append(resolve(idmap, (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF) or f"C_{(p2.read_u64(handle, cp+0x18) or 0)&0xFFFFFFFF}")
        cp = p2.read_u64(handle, cp + 0x30)
    return out

def get_live(offsets_path):
    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(offsets_path)
    mod, modules = p2.get_target_module(handle, offsets.get("engineModule", "ShooterGame.exe"))
    return handle, mod["base"], offsets, [(m["base"], m["size"], m["name"]) for m in modules]

def full_table(handle, base, offsets):
    rva = int(offsets["actor"]["fnaNameTable"]["rva"], 16)
    delta = offsets["actor"]["nameLayout"]["poolObjectDelta"]
    pool = p2.read_u64(handle, base + rva) - delta
    dirfirst = pool + int(offsets["actor"]["nameLayout"].get("dirOffset", "0x1C000"), 16)
    blob = p2.read_memory(handle, dirfirst, 0x20000) or b""
    idmap, seen = {}, set()
    for i in range(0, len(blob) - 8, 8):
        v = struct.unpack_from("<Q", blob, i)[0]
        if not (0x10000 <= v < 0x800000000000 and not v & 7): continue
        m = query_region(handle, v)
        if not m or not readable(m): continue
        if m.AllocationBase in seen: continue
        seen.add(m.AllocationBase)
        for idf, name in walk_blob(read_regions(handle, m.AllocationBase, 256 * 1024)):
            idmap.setdefault(idf, name)
    return idmap, len(seen)

def iter_actors(handle, base, offsets):
    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    level = p2.read_u64(handle, world + int(offsets["actor"]["actorArrayLevelOffset"], 16))
    arr = level + int(offsets["actor"]["actorArrayOffset"], 16)
    data, count = p2.read_u64(handle, arr), p2.read_i32(handle, arr + 8) or 0
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if a: yield a

def find_fstrings(handle, obj, lo=0x0, hi=0x400):
    out = []
    for off in range(lo, hi, 8):
        ptr = p2.read_u64(handle, obj + off)
        ln = p2.read_i32(handle, obj + off + 8)
        cap = p2.read_i32(handle, obj + off + 12)
        if not ln or not (0 < ln <= 48) or not cap or cap < ln or not ptr: continue
        if ptr & 7 or not query_region(handle, ptr): continue
        raw = p2.read_memory(handle, ptr, ln * 2) or b""
        try: s = raw.decode("utf-16-le")
        except Exception: continue
        if all(32 <= ord(c) < 127 for c in s): out.append({"offset": hex(off), "value": s})
    return out

def cmd_full(args):
    handle, base, offsets, ranges = get_live(args.offsets)
    idmap, blocks = full_table(handle, base, offsets)
    controller = pstate = own_pawn = None
    pawns = []
    for a in iter_actors(handle, base, offsets):
        cp = p2.read_u64(handle, a + 0x10)
        cj = "|".join(chain(handle, idmap, cp))
        if controller is None and "PlayerController" in cj: controller = a
        if pstate is None and "PlayerState" in cj: pstate = a
        if len(pawns) < 6 and "Character" in cj and "PlayerController" not in cj:
            pawns.append((a, cj))
    ps_off = None
    if controller and pstate:
        for off in range(0x0, 0x600, 8):
            if p2.read_u64(handle, controller + off) == pstate: ps_off = hex(off); break
    if controller:
        for off in range(0x0, 0x600, 8):
            t = p2.read_u64(handle, controller + off)
            if t and "Character" in "|".join(chain(handle, idmap, p2.read_u64(handle, t + 0x10))):
                own_pawn = t; break
    caps = {}
    for label, obj in [("own_pawn", own_pawn)] + [(f"pawn{i}", a) for i, (a, _) in enumerate(pawns)]:
        if not obj: continue
        capc = p2.read_u64(handle, obj + 0x268)
        if capc: caps[label] = {"capsule": hex(capc),
                                "floats_0x118_0x134": p2.read_floats(handle, capc + 0x118, 8)}
    # camera position from Part-2 matrix chain
    cam = None
    try:
        eng = p2.read_u64(handle, base + int(offsets["engineGlobal"]["rva"], 16))
        gi = p2.read_u64(handle, eng + int(offsets["gameInstanceOffset"], 16))
        lp = p2.read_u64(handle, p2.read_u64(handle, gi + int(offsets["localPlayersOffset"], 16)))
        vc = p2.read_u64(handle, lp + int(offsets["viewportClientOffset"], 16))
        vp = p2.read_u64(handle, vc + int(offsets["viewportOffset"], 16))
        m = p2.read_floats(handle, vp + int(offsets["viewMatrixOffset"], 16), 16)
        cam = [m[12], m[13], m[14]]
    except Exception: pass
    out = {"tableSize": len(idmap), "blocksWalked": blocks,
           "controller": hex(controller) if controller else None,
           "playerState": hex(pstate) if pstate else None,
           "controllerPlayerStateOffset": ps_off,
           "playerStateStrings": find_fstrings(handle, pstate) if pstate else [],
           "ownPawn": hex(own_pawn) if own_pawn else None,
           "capsuleCheck": caps, "cameraPosition": cam}
    json.dump(out, open(args.out, "w"), indent=2); print(json.dumps(out, indent=2))

def cmd_snap(args):
    handle, base, offsets, _ = get_live(args.offsets)
    idmap, _ = full_table(handle, base, offsets)
    controller = None
    for a in iter_actors(handle, base, offsets):
        if "PlayerController" in "|".join(chain(handle, idmap, p2.read_u64(handle, a + 0x10))):
            controller = a; break
    pawn = None
    for off in range(0x0, 0x600, 8):
        t = p2.read_u64(handle, controller + off)
        if t and "Character" in "|".join(chain(handle, idmap, p2.read_u64(handle, t + 0x10))):
            pawn = t; break
    vals = {hex(o): p2.read_floats(handle, pawn + o, 1)[0] for o in range(0x0, 0x1000, 4)}
    json.dump({"pawn": hex(pawn), "floats": vals}, open(args.out, "w"), indent=2)
    print("snapshot saved:", args.out, "pawn", hex(pawn))

def cmd_diff(args):
    handle, base, offsets, _ = get_live(args.offsets)
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
        p.add_argument("--out", default="work/probe2.json")
        p.add_argument("--snap", default="work/hp_snap.json")
        p.set_defaults(func=fn)
    a = ap.parse_args(); a.func(a)

if __name__ == "__main__": main()