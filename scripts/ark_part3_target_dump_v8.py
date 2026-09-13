#!/usr/bin/env python3
r"""Part 3 target dump v8: canonical target table, player names, robust classification.

Usage:
python scripts/ark_part3_target_dump_v8.py --out work/targets_v8.json
python scripts/ark_part3_target_dump_v8.py --player-name "YourExactPlayerName"

Outputs:
- console table
- JSON target dump
"""

import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ark_part2 as p2
from ark_part3_class_dump import (
    build_name_table,
    resolve_name,
    iter_regions,
    readable,
    MEM_PRIVATE,
    MEM_MAPPED,
)


def pint(v):
    """Parse int from JSON offset value. Accepts int or '0x123' strings."""
    if v is None:
        return None
    if isinstance(v, int):
        return v
    s = str(v).strip()
    if not s:
        return None
    return int(s, 0)


def get_live(offsets_path):
    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(
        proc["pid"],
        p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION
    )
    offsets = p2.load_offsets(offsets_path)
    mod, modules = p2.get_target_module(
        handle,
        offsets.get("engineModule", "ShooterGame.exe")
    )
    return (
        handle,
        mod["base"],
        offsets,
        [(m["base"], m["size"], m["name"]) for m in modules],
    )


def chain(handle, idmap, cp, depth=12, super_off=0x30):
    """Walk class pointer -> super struct chain and resolve class names."""
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen:
            break
        seen.add(cp)

        # FName index is lower 32 bits at UObject + nameOffset, usually +0x18.
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        out.append(resolve_name(idmap, idx) or f"id_{idx}")

        cp = p2.read_u64(handle, cp + super_off)

    return out


def read_fstring(handle, obj, off, max_len=256):
    """Read UE4-style UTF-16 FString field at obj+off.

    Layout assumed:
      +0x00: wchar_t* Data
      +0x08: int32 NumChars
      +0x0c: int32 Capacity
    """
    if not obj or off is None:
        return None

    ptr = p2.read_u64(handle, obj + off)
    ln = p2.read_i32(handle, obj + off + 8)
    cap = p2.read_i32(handle, obj + off + 12)

    if not ptr or ptr & 7:
        return None

    if ln is None or cap is None:
        return None

    if not (0 < ln <= max_len) or cap < ln:
        return None

    if not p2.query_region(handle, ptr):
        return None

    raw = p2.read_memory(handle, ptr, ln * 2)
    if not raw:
        return None

    try:
        s = raw.decode("utf-16-le").rstrip("\x00")
    except Exception:
        return None

    if not s:
        return None

    # Prefer clean ASCII, but allow printable Unicode if needed.
    if all(32 <= ord(c) < 127 for c in s):
        return s

    if all(c.isprintable() for c in s):
        return s

    return None


def scan_fstrings(handle, obj, lo=0, hi=0x1800, max_len=256):
    """Scan an object for plausible UTF-16 FString fields."""
    out = []
    if not obj:
        return out

    for off in range(lo, hi, 8):
        s = read_fstring(handle, obj, off, max_len)
        if s:
            out.append({"offset": hex(off), "value": s})

    return out


def looks_like_name(s):
    if not s or not (3 <= len(s) <= 64):
        return False

    if not any(c.isalpha() for c in s):
        return False

    if any(ch in s for ch in "\\/<>[]{}()=,;"):
        return False

    low = s.lower()
    if low.startswith(("0x", "id_", "c_", "default__")):
        return False

    return True


def get_loc(handle, a, loc_off, caps_off, comp_loc_off, ranges):
    """Read actor location.

    Primary: pawn/actor direct location offset.
    Fallback: capsule component + component location offset.
    """
    if loc_off is not None:
        loc = p2.read_floats(handle, a + loc_off, 3)
        if (
            loc
            and len(loc) == 3
            and all(
                x is not None and math.isfinite(x) and abs(x) < 1e7
                for x in loc
            )
        ):
            return loc

    if caps_off is not None:
        c = p2.read_u64(handle, a + caps_off)
        if c and p2.is_valid_ptr(handle, c, ranges) and comp_loc_off is not None:
            loc = p2.read_floats(handle, c + comp_loc_off, 3)
            if (
                loc
                and len(loc) == 3
                and all(
                    x is not None and math.isfinite(x) and abs(x) < 1e7
                    for x in loc
                )
            ):
                return loc

    return None


def read_health_value(handle, a, off, size=4, typ="float32"):
    """Read health/max-health, supporting float32 and integer fallbacks."""
    if a is None or off is None:
        return None

    if typ and "float" in str(typ).lower():
        vals = p2.read_floats(handle, a + off, 1)
        if not vals:
            return None
        v = vals[0]
        if v is None or not math.isfinite(v):
            return None
        return v

    if size == 4:
        return p2.read_i32(handle, a + off)

    if size == 8:
        return p2.read_u64(handle, a + off)

    return None


def get_camera_from_matrix(handle, base, offsets):
    """Get camera location from LocalPlayer -> ViewportClient -> ViewMatrix.

    Your validated matrix layout:
      row-major 4x4 camera->world transform
      last row = position => indices 12,13,14
    """
    try:
        engine = p2.read_u64(
            handle,
            base + pint(offsets["engineGlobal"]["rva"])
        )
        gi = p2.read_u64(
            handle,
            engine + pint(offsets["gameInstanceOffset"])
        )
        lparr = p2.read_u64(
            handle,
            gi + pint(offsets["localPlayersOffset"])
        )
        lp = p2.read_u64(handle, lparr) if lparr else None
        if not lp:
            return None

        vc = p2.read_u64(
            handle,
            lp + pint(offsets["viewportClientOffset"])
        )
        if not vc:
            return None

        m = p2.read_floats(
            handle,
            vc + pint(offsets["viewMatrixOffset"]),
            16
        )
        if not m or len(m) < 15:
            return None

        pos = (m[12], m[13], m[14])
        if all(x is not None and math.isfinite(x) and abs(x) < 1e8 for x in pos):
            return pos

        return None
    except Exception:
        return None


def find_local_controller(
    handle,
    base,
    offsets,
    ranges,
    controller_candidates,
    pstates,
    creatures,
):
    """Find local PlayerController.

    Preferred:
      engineGlobal -> GameInstance -> LocalPlayers[0] -> PlayerController

    If localPlayerControllerOffset is known, use it directly.
    Else scan LocalPlayer object for a controller pointer.

    Fallback:
      find a PlayerController with valid PlayerState and pawn offsets.
    """
    controller = None
    lp = None
    lpco_used = None
    cset = set(controller_candidates)

    try:
        engine = p2.read_u64(
            handle,
            base + pint(offsets["engineGlobal"]["rva"])
        )
        gi = p2.read_u64(
            handle,
            engine + pint(offsets["gameInstanceOffset"])
        )
        lparr = p2.read_u64(
            handle,
            gi + pint(offsets["localPlayersOffset"])
        )
        lp = p2.read_u64(handle, lparr) if lparr else None
    except Exception:
        lp = None

    if lp:
        lpco_off = pint(offsets.get("localPlayerControllerOffset"))
        if lpco_off is None:
            lpco_off = pint(
                (offsets.get("localPlayer") or {}).get("playerControllerOffset")
            )

        if lpco_off is not None:
            c = p2.read_u64(handle, lp + lpco_off)
            if c in cset:
                controller = c
                lpco_used = hex(lpco_off)

        if controller is None:
            blob = p2.read_memory(handle, lp, 0x8000)
            if blob:
                for c in controller_candidates:
                    j = blob.find(struct.pack("<Q", c))
                    if j >= 0 and j % 8 == 0:
                        controller = c
                        lpco_used = hex(j)
                        break

    if controller:
        return controller, lp, lpco_used

    # Fallback: identify a controller by known member offsets.
    A = offsets.get("actor", {})
    C = A.get("controller", {})
    ps_off = pint(C.get("playerStateOffset"))
    pawn_off = pint(C.get("pawnOffset"))
    pset = set(pstates)
    crset = set(creatures)

    for c in controller_candidates:
        if ps_off is None or pawn_off is None:
            break

        ps = p2.read_u64(handle, c + ps_off)
        pawn = p2.read_u64(handle, c + pawn_off)

        if ps in pset and pawn in crset:
            return c, lp, None

    return None, lp, None


def get_own_pawn(handle, controller, offsets, creatures, ranges):
    """Get local pawn from controller, using known controller.pawnOffset."""
    if not controller:
        return None

    A = offsets.get("actor", {})
    pawn_off = pint(A.get("controller", {}).get("pawnOffset"))

    if pawn_off is not None:
        p = p2.read_u64(handle, controller + pawn_off)
        if p and p2.is_valid_ptr(handle, p, ranges):
            return p

    # Fallback: scan controller for a creature pointer.
    blob = p2.read_memory(handle, controller, 0x2000)
    if blob:
        for c in creatures:
            j = blob.find(struct.pack("<Q", c))
            if j >= 0 and j % 8 == 0:
                return c

    return None


def get_controller_player_state(handle, controller, offsets, pstates):
    if not controller:
        return None

    A = offsets.get("actor", {})
    ps_off = pint(A.get("controller", {}).get("playerStateOffset"))
    if ps_off is None:
        return None

    ps = p2.read_u64(handle, controller + ps_off)
    if ps and (not pstates or ps in set(pstates)):
        return ps

    return None


def object_name(handle, idmap, a):
    """Resolve actor instance/object FName at actor+0x18."""
    idx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
    name = resolve_name(idmap, idx)
    if name and all(32 <= ord(c) < 127 for c in name):
        return name
    return None


def best_actor_name(handle, idmap, a, ch):
    """Prefer object name, then class name, then id fallback."""
    name = object_name(handle, idmap, a)
    if name:
        return name

    if ch:
        cls = ch[0]
        if cls and all(32 <= ord(c) < 127 for c in cls):
            return cls

    idx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
    return f"id_{idx}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--out", default="work/targets_v8.json")
    ap.add_argument("--radius", type=float, default=60000.0)
    ap.add_argument("--max-gb", type=float, default=48.0)
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument(
        "--player-name",
        default=None,
        help="Exact in-game player name; helps choose PlayerState name candidate."
    )
    ap.add_argument(
        "--no-scan-player-names",
        action="store_true",
        help="Do not runtime-scan PlayerState for name if nameOffset is null."
    )
    ap.add_argument(
        "--blob-size",
        type=lambda x: int(x, 0),
        default=0x2000
    )
    args = ap.parse_args()

    handle, base, offsets, ranges = get_live(args.offsets)

    A = offsets.get("actor", {})
    CH = A.get("character", {})

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    # World -> PersistentLevel -> Actor TArray
    world = p2.read_u64(
        handle,
        base + pint(offsets["gworld"]["rva"])
    )
    level = p2.read_u64(
        handle,
        world + pint(A["actorArrayLevelOffset"])
    )
    arr = level + pint(A["actorArrayOffset"])
    data = p2.read_u64(handle, arr)
    count = p2.read_i32(handle, arr + 8) or 0

    class_off = pint(A.get("uobjectHeader", {}).get("classOffset", "0x10")) or 0x10
    super_off = pint(A.get("classSuperOffset", "0x30")) or 0x30

    info = []
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges):
            continue

        cp = p2.read_u64(handle, a + class_off)
        ch = chain(handle, idmap, cp, super_off=super_off)
        info.append((a, ch, "|".join(ch)))

    chain_by_actor = {a: ch for a, ch, j in info}
    joined_by_actor = {a: j for a, ch, j in info}

    pstates = [a for a, ch, j in info if ch and "PlayerState" in j]
    controllers = [a for a, ch, j in info if ch and "PlayerController" in j]
    creatures = [
        a for a, ch, j in info
        if ch and (("Character" in j) or ("Pawn" in j))
    ]

    controller, lp, lpco_used = find_local_controller(
        handle,
        base,
        offsets,
        ranges,
        controllers,
        pstates,
        creatures,
    )

    own = get_own_pawn(handle, controller, offsets, creatures, ranges)
    local_ps = get_controller_player_state(handle, controller, offsets, pstates)

    # Build pawn -> PlayerState map from all PlayerControllers.
    pawn_to_ps = {}
    ps_off = pint(A.get("controller", {}).get("playerStateOffset"))
    pawn_off = pint(A.get("controller", {}).get("pawnOffset"))

    if ps_off is not None and pawn_off is not None:
        pset = set(pstates)
        cset = set(creatures)

        for c in controllers:
            pawn = p2.read_u64(handle, c + pawn_off)
            ps = p2.read_u64(handle, c + ps_off)

            if pawn and ps and pawn in cset and ps in pset:
                pawn_to_ps[pawn] = ps

    if own and local_ps:
        pawn_to_ps[own] = local_ps

    # Member offsets
    caps_off = pint(A.get("capsuleOffset", CH.get("capsuleOffset")))
    hh_off = pint(CH.get("capsuleHalfHeightOffset", A.get("capsuleHalfHeightOffset")))
    loc_off = pint(CH.get("locationOffset", A.get("locationOffset")))
    comp_loc_off = pint(
        CH.get("componentLocationOffset", A.get("componentLocationOffset", "0xf0"))
    )
    hp_off = pint(CH.get("healthOffset", A.get("healthOffset")))
    mhp_off = pint(CH.get("maxHealthOffset", A.get("maxHealthOffset")))
    hp_size = pint(CH.get("healthSize", A.get("healthSize", 4)))
    hp_type = CH.get("healthType", A.get("healthType", "float32"))

    # Camera priority:
    #   1. own pawn location
    #   2. viewport matrix position
    #   3. first valid creature location
    #   4. origin
    cam = None

    if own:
        cam = get_loc(handle, own, loc_off, caps_off, comp_loc_off, ranges)

    if not cam:
        cam = get_camera_from_matrix(handle, base, offsets)

    if not cam:
        for a in creatures:
            loc = get_loc(handle, a, loc_off, caps_off, comp_loc_off, ranges)
            if loc:
                cam = loc
                break

    if not cam:
        cam = (0.0, 0.0, 0.0)

    ps_name_cache = {}
    discovery = {}
    ps_name_debug = {}

    def score_name(s):
        """Score a possible PlayerState name.

        Higher score = more likely to be the real player name.
        This does not hardcode a specific name.
        """
        if not s or not (2 <= len(s) <= 64):
            return -100000

        if not any(c.isalpha() for c in s):
            return -100000

        if any(ch in s for ch in "\\/<>[]{}()=,;"):
            return -100000

        score = 0
        low = s.lower()

        # If the user supplies an exact preferred name, strongly prefer it.
        if args.player_name and low == args.player_name.lower():
            score += 1000

        # Real player names often contain digits or mixed case.
        if any(ch.isdigit() for ch in s):
            score += 80

        if len(s) >= 6:
            score += 20

        # Avoid tribe strings.
        if low.startswith("tribe"):
            score -= 200

        # Avoid generic species/class strings.
        if low in ("human", "player", "unknown", "default", "test", "debug"):
            score -= 50

        return score

    def get_ps_name(ps):
        """Read PlayerState name dynamically.

        Priority:
          1. actor.playerState.nameOffset
          2. actor.playerState.nameOffsetCandidates
          3. runtime FString scan
        """
        if ps in ps_name_cache:
            return ps_name_cache[ps]

        name = None
        best_score = -100000

        PS = A.get("playerState", {})

        offsets_to_try = []

        primary = pint(PS.get("nameOffset"))
        if primary is not None:
            offsets_to_try.append(primary)

        for cand in PS.get("nameOffsetCandidates", []):
            o = pint(cand)
            if o is not None and o not in offsets_to_try:
                offsets_to_try.append(o)

        # Try fixed/candidate offsets first.
        for off in offsets_to_try:
            s = read_fstring(handle, ps, off)
            if not s:
                continue

            sc = score_name(s)
            if sc > best_score:
                best_score = sc
                name = s

        # If fixed offsets did not produce a good name, scan the PlayerState.
        if (name is None or best_score < 0) and not args.no_scan_player_names:
            scan_range = PS.get("nameScanRange", ["0x0", "0x1800"])
            lo = pint(scan_range[0]) if len(scan_range) > 0 else 0
            hi = pint(scan_range[1]) if len(scan_range) > 1 else 0x1800

            cands = scan_fstrings(handle, ps, lo, hi)

            for c in cands:
                s = c.get("value")
                if not s:
                    continue

                sc = score_name(s)
                if sc > best_score:
                    best_score = sc
                    name = s

                    if ps == local_ps:
                        discovery["playerStateNameOffsetCandidate"] = c["offset"]

        ps_name_cache[ps] = name
        return name

    def class_kind(j):
        cl = A.get("classification", {})
        player_markers = cl.get(
            "playerMarkers",
            ["ShooterCharacter", "PlayerController", "PlayerState"]
        )
        dino_markers = cl.get(
            "dinoMarkers",
            ["PrimalDinoCharacter", "DinoCharacter", "PrimalCharacter"]
        )

        if any(m in j for m in dino_markers):
            return "dino"

        if any(m in j for m in player_markers):
            return "player"

        if "Pawn" in j:
            return "pawn"

        return "actor"

    targets = []

    pstate_packed = [(ps, struct.pack("<Q", ps)) for ps in pstates]
    controller_packed = [(c, struct.pack("<Q", c)) for c in controllers]
    ownership_packed = [pb for _, pb in pstate_packed + controller_packed]

    for a in creatures:
        ch = chain_by_actor.get(a, [])
        j = joined_by_actor.get(a, "")

        loc = get_loc(handle, a, loc_off, caps_off, comp_loc_off, ranges)
        if not loc:
            continue

        dist = math.dist(loc, cam)
        if dist > args.radius:
            continue

        kind = class_kind(j)

        is_player = (a == own) or (kind == "player")
        label = "player" if is_player else "wild"

        blob = None

        # Dino/creature ownership:
        # If creature object contains a PlayerState or PlayerController pointer,
        # classify as tamed. Players are handled separately.
        if not is_player:
            blob = p2.read_memory(handle, a, args.blob_size) or b""
            owned = False

            for pb in ownership_packed:
                if pb in blob:
                    owned = True
                    break

            label = "tamed" if owned else "wild"

        # Player names come from PlayerState.
        name = None

        if is_player:
            ps = pawn_to_ps.get(a)

            if not ps and a == own:
                ps = local_ps

            # Fallback: scan pawn blob for a PlayerState pointer.
            if not ps:
                blob = blob or p2.read_memory(handle, a, args.blob_size) or b""
                best_off = None

                for psv, pb in pstate_packed:
                    idx = blob.find(pb)
                    if idx >= 0 and idx % 8 == 0:
                        if best_off is None or idx < best_off:
                            ps = psv
                            best_off = idx

            if ps:
                name = get_ps_name(ps)

        if not name:
            name = best_actor_name(handle, idmap, a, ch)

        caps = p2.read_u64(handle, a + caps_off) if caps_off is not None else None
        hh = None

        if caps and p2.is_valid_ptr(handle, caps, ranges) and hh_off is not None:
            vals = p2.read_floats(handle, caps + hh_off, 1)
            hh = vals[0] if vals else None

        hp = read_health_value(handle, a, hp_off, hp_size, hp_type)
        mhp = read_health_value(handle, a, mhp_off, hp_size, hp_type)

        targets.append({
            "ptr": hex(a),
            "worldX": loc[0],
            "worldY": loc[1],
            "worldZ": loc[2],
            "halfHeight": hh,
            "health": hp,
            "maxHealth": mhp,
            "name": name,
            "isPlayer": is_player,
            "classLabel": label,
            "classKind": kind,
            "distance": round(dist, 1),
            "classChain": ch[:6],
        })

    # Stable ordering: distance first, then pointer.
    targets.sort(key=lambda t: (t["distance"], t["ptr"]))

    counts = {}
    for t in targets:
        counts[t["classLabel"]] = counts.get(t["classLabel"], 0) + 1

    out = {
        "camera": list(cam),
        "tableSize": len(idmap),
        "counts": counts,
        "count": len(targets),
        "structural_lock": {
            "ownPawn": hex(own) if own else None,
            "controller": hex(controller) if controller else None,
            "playerState": hex(local_ps) if local_ps else None,
            "localPlayerControllerOffset": lpco_used,
        },
        "missing": [],
        "discovery": discovery,
        "playerStateNameCandidates": ps_name_debug,
        "targets": targets,
    }

    if pint(A.get("playerState", {}).get("nameOffset")) is None:
        out["missing"].append("actor.playerState.nameOffset")

    if not lpco_used:
        out["missing"].append("localPlayerControllerOffset")

    d = os.path.dirname(os.path.abspath(args.out))
    if d:
        os.makedirs(d, exist_ok=True)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    for t in targets[:args.limit]:
        hp = "-" if t["health"] is None else f"{t['health']:.1f}"
        mhp = "-" if t["maxHealth"] is None else f"{t['maxHealth']:.1f}"
        hh = "-" if t["halfHeight"] is None else f"{t['halfHeight']:.1f}"

        print(
            f"{t['distance']:>10.1f}  "
            f"{t['classLabel']:<6} "
            f"{hp:>8}/{mhp:<8} "
            f"hh={hh:>7} "
            f"{t['name']}"
        )

    print("...", json.dumps(counts))

    if out["missing"]:
        print("missing:", json.dumps(out["missing"]))

    if discovery:
        print("discovery:", json.dumps(discovery))


if __name__ == "__main__":
    main()