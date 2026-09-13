#!/usr/bin/env python3
r"""Part 3 probe3: controller/pawn/playerstate via localPlayer chain. Singleplayer/local only.
Usage:
  python scripts/ark_part3_probe3.py full  --out work/probe3.json
  python scripts/ark_part3_probe3.py snap  --out work/hp_snap.json
  (take damage in game)
  python scripts/ark_part3_probe3.py diff  --snap work/hp_snap.json
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import build_name_table, resolve_name, iter_regions, readable, MEM_PRIVATE, MEM_MAPPED


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


def fstrings(handle, obj, lo=0x0, hi=0x400):
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


def get_live(offsets_path):
    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(offsets_path)
    mod, modules = p2.get_target_module(handle, offsets.get("engineModule", "ShooterGame.exe"))
    return handle, mod["base"], offsets, [(m["base"], m["size"], m["name"]) for m in modules]


def cmd_full(args):
    handle, base, offsets, ranges = get_live(args.offsets)
    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    engine = p2.read_u64(handle, base + int(offsets["engineGlobal"]["rva"], 16))
    gi = p2.read_u64(handle, engine + int(offsets["gameInstanceOffset"], 16))
    lpa = gi + int(offsets["localPlayersOffset"], 16)
    lp = p2.read_u64(handle, p2.read_u64(handle, lpa))
    out = {"tableSize": len(idmap), "localPlayer": hex(lp) if lp else None}

    controller = ctrl_off = None
    if lp:
        for off in range(0x0, 0x400, 8):
            t = p2.read_u64(handle, lp + off)
            if not t or not p2.is_valid_ptr(handle, t, ranges):
                continue
            if any("Controller" in n for n in chain(handle, idmap, p2.read_u64(handle, t + 0x10))):
                controller, ctrl_off = t, hex(off)
                break
    out["controller"] = hex(controller) if controller else None
    out["localPlayerControllerOffset"] = ctrl_off

    pawn = pawn_off = pstate = ps_off = None
    if controller:
        for off in range(0x0, 0x800, 8):
            t = p2.read_u64(handle, controller + off)
            if not t or not p2.is_valid_ptr(handle, t, ranges):
                continue
            cj = chain(handle, idmap, p2.read_u64(handle, t + 0x10))
            if pstate is None and any("PlayerState" in n for n in cj):
                pstate, ps_off = t, hex(off)
            if pawn is None and any("Character" in n for n in cj):
                pawn, pawn_off = t, hex(off)
            if pawn and pstate:
                break
    out.update({"controllerPlayerStateOffset": ps_off, "controllerPawnOffset": pawn_off,
                "playerState": hex(pstate) if pstate else None,
                "pawn": hex(pawn) if pawn else None})

    if pstate:
        out["playerStateStrings"] = fstrings(handle, pstate)
    if pawn:
        caps = p2.read_u64(handle, pawn + 0x268)
        root = p2.read_u64(handle, pawn + 0x170)
        out["capsuleFloats"] = {hex(o): p2.read_floats(handle, caps + o, 4)
                                for o in range(0x110, 0x140, 0x10)} if caps else None
        out["rootLoc"] = p2.read_floats(handle, root + 0xf0, 3) if root else None
        info = {"pawn": hex(pawn)}
        d = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "pawn_info.json"), "w") as f:
            json.dump(info, f)

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
        p.add_argument("--out", default="work/probe3.json")
        p.add_argument("--snap", default="work/hp_snap.json")
        p.set_defaults(func=fn)
    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()