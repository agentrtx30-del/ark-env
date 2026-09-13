#!/usr/bin/env python3
r"""
ARK Part 3 — pawn/capsule/float-grid probe. Singleplayer/local diagnostic only.
Usage: python scripts/ark_part3_pawn_probe.py --offsets offsets/ark_offsets.json --out work/pawn_probe.json
"""
import argparse, ctypes, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import (build_name_table, resolve_name, iter_regions,
                                  readable, MEM_PRIVATE, MEM_MAPPED)

MEM_IMAGE = 0x01000000


def class_chain(handle, idmap, cp, depth=8):
    names, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen:
            break
        seen.add(cp)
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        names.append(resolve_name(idmap, idx) or f"C_{idx}")
        cp = p2.read_u64(handle, cp + 0x30)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--max-gb", type=float, default=48.0)
    ap.add_argument("--out", default="work/pawn_probe.json")
    args = ap.parse_args()

    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(args.offsets)
    mod, modules = p2.get_target_module(handle, offsets.get("engineModule", "ShooterGame.exe"))
    base = mod["base"]
    module_ranges = [(m["base"], m["size"], m["name"]) for m in modules]

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    actor = offsets["actor"]
    level = p2.read_u64(handle, world + int(actor["actorArrayLevelOffset"], 16))
    arr = level + int(actor["actorArrayOffset"], 16)
    data = p2.read_u64(handle, arr)
    count = p2.read_i32(handle, arr + 8) or 0

    def actor_chain(a):
        cp = p2.read_u64(handle, a + 0x10)
        return class_chain(handle, idmap, cp) if cp else []

    controller = pawn = pstate = None
    actors = []
    for i in range(min(count, 1500)):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, module_ranges):
            continue
        chain = actor_chain(a)
        joined = "|".join(chain)
        if controller is None and "PlayerController" in joined:
            controller = a
        if pstate is None and "PlayerState" in joined:
            pstate = a
        actors.append((a, chain))

    # pawn + playerstate from controller scan
    if controller:
        for off in range(0x00, 0x600, 8):
            t = p2.read_u64(handle, controller + off)
            if not t or not p2.is_valid_ptr(handle, t, module_ranges):
                continue
            cj = "|".join(actor_chain(t))
            if pawn is None and ("Character" in cj or "Pawn" in cj):
                pawn = t
            if "PlayerState" in cj and not hasattr(main, "_ps"):
                pass

    def find_comp(obj, lo, hi, marker):
        for off in range(lo, hi, 8):
            t = p2.read_u64(handle, obj + off)
            if not t or not p2.is_valid_ptr(handle, t, module_ranges):
                continue
            cn = class_chain(handle, idmap, p2.read_u64(handle, t + 0x10), 2)
            if cn and marker in cn[0]:
                return t, off, cn[0]
        return None, None, None

    out = {"controller": hex(controller) if controller else None,
           "pawn": hex(pawn) if pawn else None,
           "playerState": hex(pstate) if pstate else None}

    if pawn:
        out["pawnChain"] = actor_chain(pawn)
        root, roff, rname = find_comp(pawn, 0x100, 0x400, "Component")
        caps, coff, cname = find_comp(pawn, 0x100, 0x800, "Capsule")
        out["root"] = {"ptr": hex(root) if root else None, "off": hex(roff) if roff is not None else None, "class": rname}
        out["capsule"] = {"ptr": hex(caps) if caps else None, "off": hex(coff) if coff is not None else None, "class": cname}
        if root:
            out["rootFloats"] = {hex(o): p2.read_floats(handle, root + o, 4)
                                 for o in range(0x100, 0x200, 0x10)}
        if caps:
            out["capsuleFloats"] = {hex(o): p2.read_floats(handle, caps + o, 4)
                                    for o in range(0x000, 0x300, 0x10)}
        out["pawnFloats"] = {hex(o): p2.read_floats(handle, pawn + o, 4)
                             for o in range(0x000, 0x800, 0x40)}

    d = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(d, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()