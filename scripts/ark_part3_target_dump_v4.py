#!/usr/bin/env python3
r"""Part 3 target dump v4. 100% structural, uses controller offsets to find player."""
import argparse, json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--out", default="work/targets.json")
    ap.add_argument("--radius", type=float, default=60000)
    args = ap.parse_args()

    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(proc["pid"], p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION)
    offsets = p2.load_offsets(args.offsets)
    mod, modules = p2.get_target_module(handle, offsets["engineModule"])
    base = mod["base"]
    ranges = [(m["base"], m["size"], m["name"]) for m in modules]
    A = offsets["actor"]; CH = A["character"]
    CTRL = A["controller"]

    # 1. Camera (with fallback to pawn location if matrix is garbage)
    cam = None
    try:
        engine = p2.read_u64(handle, base + int(offsets["engineGlobal"]["rva"], 16))
        gi = p2.read_u64(handle, engine + int(offsets["gameInstanceOffset"], 16))
        lp = p2.read_u64(handle, p2.read_u64(handle, gi + int(offsets["localPlayersOffset"], 16)))
        vc = p2.read_u64(handle, lp + int(offsets["viewportClientOffset"], 16))
        m16 = p2.read_floats(handle, vc + int(offsets["viewMatrixOffset"], 16), 16)
        if m16 and all(x is not None and abs(x) < 10000000 for x in m16):
            cam = (m16[12], m16[13], m16[14])
    except Exception:
        pass

    # 2. Actors
    world = p2.read_u64(handle, base + int(offsets["gworld"]["rva"], 16))
    level = p2.read_u64(handle, world + int(A["actorArrayLevelOffset"], 16))
    arr = level + int(A["actorArrayOffset"], 16)
    data = p2.read_u64(handle, arr); count = p2.read_i32(handle, arr+8) or 0

    actors = []
    for i in range(count):
        a = p2.read_u64(handle, data + i*8)
        if a and p2.is_valid_ptr(handle, a, ranges): actors.append(a)
    actor_set = set(actors)

    # 3. Find Pawns (Capsule halfHeight between 40 and 90)
    pawns, pawn_capsules = [], {}
    for a in actors:
        caps = p2.read_u64(handle, a + int(A["capsuleOffset"], 16))
        if caps and p2.is_valid_ptr(handle, caps, ranges):
            hh = p2.read_floats(handle, caps + int(CH["capsuleHalfHeightOffset"], 16), 1)[0]
            if hh and 40.0 < hh < 90.0:
                pawns.append(a); pawn_capsules[a] = caps

    pawn_set = set(pawns)

    # 4. Find PlayerController structurally using proven offsets (0x490 / 0x498)
    controller, own_pawn, player_state = None, None, None
    ctrl_pawn_off = int(CTRL["pawnOffset"], 16)
    ctrl_ps_off = int(CTRL["playerStateOffset"], 16)

    for a in actors:
        p_ptr = p2.read_u64(handle, a + ctrl_pawn_off)
        ps_ptr = p2.read_u64(handle, a + ctrl_ps_off)
        # If it points to a valid pawn AND a valid actor (PlayerState), it's the Controller
        if p_ptr in pawn_set and ps_ptr and ps_ptr in actor_set:
            controller = a
            own_pawn = p_ptr
            player_state = ps_ptr
            break

    if not own_pawn:
        print("FATAL: Could not find PlayerController structurally.")
        sys.exit(1)

    # Fallback camera to own_pawn location if matrix read failed
    if not cam:
        loc = p2.read_floats(handle, own_pawn + int(CH["locationOffset"], 16), 3)
        if loc and all(x is not None for x in loc):
            cam = tuple(loc)
        else:
            cam = (0, 0, 0)

    ps_packed = struct.pack("<Q", player_state) if player_state else b""
    ctrl_packed = struct.pack("<Q", controller) if controller else b""
    pawn_packed = struct.pack("<Q", own_pawn) if own_pawn else b""

    # 5. Build Targets
    targets = []
    for a in pawns:
        caps = pawn_capsules[a]
        hh = p2.read_floats(handle, caps + int(CH["capsuleHalfHeightOffset"], 16), 1)[0]
        rad = p2.read_floats(handle, caps + int(CH["capsuleRadiusOffset"], 16), 1)[0]
        loc = p2.read_floats(handle, a + int(CH["locationOffset"], 16), 3)
        if not loc or any(x is None for x in loc): continue
        
        dist = math.dist(loc, cam)
        
        if a == own_pawn: 
            label = "player"
        else:
            blob = p2.read_memory(handle, a, 0x2000) or b""
            # Tamed if it references the player state, controller, or own pawn
            if (player_state and ps_packed in blob) or (controller and ctrl_packed in blob) or (own_pawn and pawn_packed in blob):
                label = "tamed"
            else:
                label = "wild"
            
            if dist > args.radius: continue

        hp = p2.read_floats(handle, a + int(CH["healthOffset"], 16), 1)[0]
        mhp = p2.read_floats(handle, a + int(CH["maxHealthOffset"], 16), 1)[0]
        targets.append({"ptr": hex(a), "label": label, "loc": loc, "distance": round(dist, 1),
                        "capsuleHalfHeight": hh, "capsuleRadius": rad, "health": hp, "maxHealth": mhp})

    targets.sort(key=lambda t: t["distance"])
    out = {"camera": list(cam), "count": len(targets), "targets": targets,
           "structural_lock": {"ownPawn": hex(own_pawn),
                               "controller": hex(controller),
                               "playerState": hex(player_state)}}
    
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    json.dump(out, open(args.out, "w"), indent=2)
    
    for t in targets[:40]:
        print(f"{t['distance']:>10.1f}  {t['label']:<6} hp={t['health']}/{t['maxHealth']} hh={t['capsuleHalfHeight']}")
    print("...")

if __name__ == "__main__":
    main()