#!/usr/bin/env python3
r"""
ARK Actor Dump — Final target tool.
Dumps all actors with their class, name, category, and world location.
Usage: python scripts/ark_actor_dump.py --offsets offsets/ark_offsets.json --out work/actors.json
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2
from ark_part3_class_dump import build_name_table, resolve_name, iter_regions, readable, MEM_PRIVATE, MEM_MAPPED

def class_chain(handle, idmap, cp, depth=8):
    names, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen: break
        seen.add(cp)
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        names.append(resolve_name(idmap, idx) or f"C_{idx}")
        cp = p2.read_u64(handle, cp + 0x30)
    return names

def classify(chain, markers):
    s = "|".join(chain)
    if any(m in s for m in markers.get("player", [])): return "player"
    if any(m in s for m in markers.get("dino", [])): return "dino"
    if any(m in s for m in markers.get("structure", [])): return "structure"
    if "PrimalItem" in s or "Item" in s: return "item"
    if "Pawn" in s or "Character" in s: return "pawn"
    if "Actor" in s: return "actor"
    return "other"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--max-gb", type=float, default=48.0)
    ap.add_argument("--out", default="work/actors.json")
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

    markers = actor.get("classification", {})
    loc_off = int(actor["component"]["locationOffset"], 16)
    
    actors = []
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, module_ranges): continue

        cp = p2.read_u64(handle, a + 0x10)
        chain = class_chain(handle, idmap, cp) if cp else []
        
        n_idx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
        actor_name = resolve_name(idmap, n_idx) or f"Unknown_{n_idx}"

        loc = None
        for comp_off in (int(actor.get("capsuleComponentOffset", "0x268"), 16), 
                         int(actor.get("rootComponentOffset", "0x170"), 16)):
            comp = p2.read_u64(handle, a + comp_off)
            if comp and p2.is_valid_ptr(handle, comp, module_ranges):
                f = p2.read_floats(handle, comp + loc_off, 3)
                if f and all(x is not None and abs(x) < 1e6 for x in f):
                    loc = f
                    break

        actors.append({
            "i": i,
            "ptr": hex(a),
            "name": actor_name,
            "class": chain[0] if chain else "Unknown",
            "category": classify(chain, markers),
            "location": loc
        })

    out = {"count": len(actors), "actors": actors}
    d = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(d, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f: json.dump(out, f, indent=2)
    print(f"✓ Dumped {len(actors)} actors to {args.out}")

if __name__ == "__main__": main()