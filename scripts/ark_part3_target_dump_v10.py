#!/usr/bin/env python3
r"""Part 3 target dump v10: repaired filtering + dynamic names + level.

New vs v9:
- reads Target.level via:
    pawn + statusComponentOffset -> UPrimalCharacterStatusComponent*
    statusComponent + levelOffset -> int32 BaseLevel
- "level" added to every target record and to the console table.

Usage:
python scripts/ark_part3_target_dump_v10.py --radius 100000 --out work/targets_v10.json
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
    out, seen = [], set()
    for _ in range(depth):
        if not cp or cp in seen:
            break
        seen.add(cp)
        idx = (p2.read_u64(handle, cp + 0x18) or 0) & 0xFFFFFFFF
        out.append(resolve_name(idmap, idx) or f"id_{idx}")
        cp = p2.read_u64(handle, cp + super_off)
    return out


def readable_ptr(handle, ptr):
    if not ptr or ptr & 7:
        return False
    try:
        return bool(p2.query_region(handle, ptr))
    except Exception:
        return False


def read_blob_chunked(handle, addr, size, chunk=0x1000):
    out = bytearray()
    off = 0
    while off < size:
        n = min(chunk, size - off)
        part = p2.read_memory(handle, addr + off, n)
        if not part:
            break
        out += part
        off += len(part)
        if len(part) < n:
            break
    return bytes(out)


def read_fstring(handle, obj, off, max_len=256):
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
    if all(32 <= ord(c) < 127 for c in s):
        return s
    if all(c.isprintable() for c in s):
        return s
    return None


def scan_fstrings(handle, obj, lo=0, hi=0x1800, max_len=256):
    out = []
    if not obj:
        return out
    for off in range(lo, hi, 8):
        s = read_fstring(handle, obj, off, max_len)
        if s:
            out.append({"offset": hex(off), "value": s})
    return out


def plausible_name(s):
    if not s or not (2 <= len(s) <= 64):
        return False
    if not any(c.isalpha() for c in s):
        return False
    if any(ch in s for ch in "\\/<>[]{}()=,;"):
        return False
    if any(ord(c) < 32 for c in s):
        return False
    return True


def score_name(s, preferred=None):
    if not plausible_name(s):
        return -1000000000
    score = 0
    low = s.lower()
    if preferred and low == preferred.lower():
        score += 1000
    if any(ch.isdigit() for ch in s):
        score += 80
    if len(s) >= 6:
        score += 20
    if low.startswith("tribe"):
        score -= 200
    if low in ("human", "player", "unknown", "default", "test", "debug"):
        score -= 80
    return score


def get_loc(handle, a, offsets, ranges, allow_zero=False):
    A = offsets.get("actor", {})
    CH = A.get("character", {})
    loc_off = pint(CH.get("locationOffset", A.get("locationOffset")))
    caps_off = pint(CH.get("capsuleOffset", A.get("capsuleOffset")))
    root_off = pint(A.get("rootComponentOffset"))
    comp_loc_off = pint(
        CH.get("componentLocationOffset", A.get("componentLocationOffset", "0xf0"))
    )

    def valid(loc):
        if not loc or len(loc) != 3:
            return False
        if not all(
            x is not None and math.isfinite(x) and abs(x) < 1e8
            for x in loc
        ):
            return False
        if not allow_zero and all(abs(x) < 1e-3 for x in loc):
            return False
        return True

    if loc_off is not None:
        loc = p2.read_floats(handle, a + loc_off, 3)
        if valid(loc):
            return loc

    for comp_off in (caps_off, root_off):
        if comp_off is None:
            continue
        comp = p2.read_u64(handle, a + comp_off)
        if not comp or not readable_ptr(handle, comp):
            continue
        if comp_loc_off is None:
            continue
        loc = p2.read_floats(handle, comp + comp_loc_off, 3)
        if valid(loc):
            return loc

    return None


def read_health_value(handle, a, off, size=4, typ="float32"):
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


def read_level(handle, a, offsets, ranges):
    """pawn + statusComponentOffset -> status component; + levelOffset -> int level."""
    A = offsets.get("actor", {})
    CH = A.get("character", {})
    status_off = pint(CH.get("statusComponentOffset", A.get("statusComponentOffset")))
    level_off = pint(CH.get("levelOffset", A.get("levelOffset")))
    level_size = pint(CH.get("levelSize", A.get("levelSize", 4))) or 4
    if status_off is None or level_off is None:
        return None
    status = p2.read_u64(handle, a + status_off)
    if not status or not readable_ptr(handle, status):
        return None
    level = None
    if level_size == 1:
        raw = p2.read_memory(handle, status + level_off, 1)
        level = raw[0] if raw else None
    elif level_size == 2:
        raw = p2.read_memory(handle, status + level_off, 2)
        level = struct.unpack("<H", raw)[0] if raw and len(raw) >= 2 else None
    elif level_size == 8:
        level = p2.read_u64(handle, status + level_off)
    else:
        level = p2.read_i32(handle, status + level_off)
    if level is None:
        return None
    if not (0 <= level <= 1000):
        return None
    return level


def get_camera_from_matrix(handle, base, offsets):
    try:
        engine = p2.read_u64(handle, base + pint(offsets["engineGlobal"]["rva"]))
        gi = p2.read_u64(handle, engine + pint(offsets["gameInstanceOffset"]))
        lparr = p2.read_u64(handle, gi + pint(offsets["localPlayersOffset"]))
        lp = p2.read_u64(handle, lparr) if lparr else None
        if not lp:
            return None
        vc = p2.read_u64(handle, lp + pint(offsets["viewportClientOffset"]))
        if not vc:
            return None
        m = p2.read_floats(handle, vc + pint(offsets["viewMatrixOffset"]), 16)
        if not m or len(m) < 15:
            return None
        pos = (m[12], m[13], m[14])
        if all(x is not None and math.isfinite(x) and abs(x) < 1e8 for x in pos):
            return pos
        return None
    except Exception:
        return None


def read_controller_state(handle, c, ps_off, pawn_off, pset, crset):
    ps = p2.read_u64(handle, c + ps_off) if ps_off is not None else None
    pawn = p2.read_u64(handle, c + pawn_off) if pawn_off is not None else None
    if ps and pawn:
        ps_ok = (not pset) or (ps in pset)
        pawn_ok = (not crset) or (pawn in crset)
        if ps_ok and pawn_ok:
            return ps, pawn
    return None, None


def find_local_controller(
    handle, base, offsets, ranges, controller_candidates, pstates, creatures
):
    A = offsets.get("actor", {})
    C = A.get("controller", {})
    ps_off = pint(C.get("playerStateOffset"))
    pawn_off = pint(C.get("pawnOffset"))
    pset = set(pstates)
    crset = set(creatures)
    cset = set(controller_candidates)

    lp = None
    try:
        engine = p2.read_u64(handle, base + pint(offsets["engineGlobal"]["rva"]))
        gi = p2.read_u64(handle, engine + pint(offsets["gameInstanceOffset"]))
        lparr = p2.read_u64(handle, gi + pint(offsets["localPlayersOffset"]))
        lp = p2.read_u64(handle, lparr) if lparr else None
    except Exception:
        lp = None

    if lp:
        lpco_off = pint(offsets.get("localPlayerControllerOffset"))
        if lpco_off is not None:
            c = p2.read_u64(handle, lp + lpco_off)
            if c:
                ps, pawn = read_controller_state(handle, c, ps_off, pawn_off, pset, crset)
                if ps and pawn:
                    return c, ps, pawn, "localPlayerControllerOffset"

        blob = read_blob_chunked(handle, lp, 0x20000)
        if blob:
            for c in controller_candidates:
                j = blob.find(struct.pack("<Q", c))
                if j >= 0 and j % 8 == 0:
                    ps, pawn = read_controller_state(
                        handle, c, ps_off, pawn_off, pset, crset
                    )
                    if ps and pawn:
                        return c, ps, pawn, f"LocalPlayer blob scan +{j:#x}"

    if ps_off is not None and pawn_off is not None:
        for c in controller_candidates:
            ps, pawn = read_controller_state(handle, c, ps_off, pawn_off, pset, crset)
            if ps and pawn:
                return c, ps, pawn, "controller member offsets"

    return None, None, None, None


def find_structural_controller(handle, actors, pstates, creatures):
    pset = set(pstates)
    cset = set(creatures)
    for ps in pstates[:8]:
        psb = struct.pack("<Q", ps)
        for a in actors:
            if a == ps or a in cset:
                continue
            blob = p2.read_memory(handle, a, 0x1000)
            if not blob or psb not in blob:
                continue
            i1 = blob.find(psb)
            if i1 % 8:
                continue
            for pawn in creatures:
                pb = struct.pack("<Q", pawn)
                i2 = blob.find(pb)
                if i2 >= 0 and i2 % 8 == 0:
                    return a, ps, pawn, f"structural PlayerState +{i1:#x}", f"structural pawn +{i2:#x}"
    return None, None, None, None, None


def get_own_pawn(handle, controller, offsets, creatures, ranges):
    if not controller:
        return None
    A = offsets.get("actor", {})
    pawn_off = pint(A.get("controller", {}).get("pawnOffset"))
    if pawn_off is not None:
        p = p2.read_u64(handle, controller + pawn_off)
        if p and p2.is_valid_ptr(handle, p, ranges):
            return p
    blob = read_blob_chunked(handle, controller, 0x4000)
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
    idx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
    return resolve_name(idmap, idx)


def best_actor_name(handle, idmap, a, ch, kind):
    obj = object_name(handle, idmap, a)
    if obj and all(32 <= ord(c) < 127 for c in obj):
        if not obj.startswith("id_"):
            return obj
    skip = {"Actor", "Pawn", "Character", "Object"}
    for n in ch:
        if not n or n.startswith("id_") or n in skip:
            continue
        if all(32 <= ord(c) < 127 for c in n):
            return n
    if obj:
        return obj
    idx = (p2.read_u64(handle, a + 0x18) or 0) & 0xFFFFFFFF
    return f"{kind}_{idx}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--out", default="work/targets_v10.json")
    ap.add_argument("--radius", type=float, default=60000.0)
    ap.add_argument("--max-gb", type=float, default=48.0)
    ap.add_argument("--limit", type=int, default=80)
    ap.add_argument("--player-name", default=None)
    ap.add_argument("--no-scan-player-names", action="store_true")
    ap.add_argument("--blob-size", type=lambda x: int(x, 0), default=0x2000)
    ap.add_argument("--include-dead", action="store_true")
    ap.add_argument("--include-zero", action="store_true")
    ap.add_argument("--include-invalid", action="store_true")
    args = ap.parse_args()

    handle, base, offsets, ranges = get_live(args.offsets)

    A = offsets.get("actor", {})
    CH = A.get("character", {})
    PS = A.get("playerState", {})

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    world = p2.read_u64(handle, base + pint(offsets["gworld"]["rva"]))
    level_obj = p2.read_u64(handle, world + pint(A["actorArrayLevelOffset"]))
    arr = level_obj + pint(A["actorArrayOffset"])
    data = p2.read_u64(handle, arr)
    count = p2.read_i32(handle, arr + 8) or 0

    class_off = pint(A.get("uobjectHeader", {}).get("classOffset", "0x10")) or 0x10
    super_off = pint(A.get("classSuperOffset", "0x30")) or 0x30

    info = []
    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges):
            continue
        ch = chain(handle, idmap, p2.read_u64(handle, a + class_off), super_off=super_off)
        info.append((a, ch, "|".join(ch)))

    chain_by_actor = {a: ch for a, ch, j in info}
    joined_by_actor = {a: j for a, ch, j in info}
    actors = [a for a, ch, j in info]

    pstates = [a for a, ch, j in info if ch and "PlayerState" in j]
    controllers = [a for a, ch, j in info if ch and "PlayerController" in j]
    creatures = [a for a, ch, j in info if ch and (("Character" in j) or ("Pawn" in j))]

    controller, local_ps, own, method = find_local_controller(
        handle, base, offsets, ranges, controllers, pstates, creatures
    )

    structural_ps_off = None
    structural_pawn_off = None

    if not controller:
        (
            controller,
            local_ps,
            own,
            structural_ps_off,
            structural_pawn_off,
        ) = find_structural_controller(handle, actors, pstates, creatures)
        if controller:
            method = "structural controller discovery"

    if controller and controller not in controllers:
        controllers.append(controller)

    if controller and not own:
        own = get_own_pawn(handle, controller, offsets, creatures, ranges)

    if controller and not local_ps:
        local_ps = get_controller_player_state(handle, controller, offsets, pstates)

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

    caps_off = pint(A.get("capsuleOffset", CH.get("capsuleOffset")))
    hh_off = pint(CH.get("capsuleHalfHeightOffset", A.get("capsuleHalfHeightOffset")))
    hp_off = pint(CH.get("healthOffset", A.get("healthOffset")))
    mhp_off = pint(CH.get("maxHealthOffset", A.get("maxHealthOffset")))
    hp_size = pint(CH.get("healthSize", A.get("healthSize", 4)))
    hp_type = CH.get("healthType", A.get("healthType", "float32"))

    own_loc = None
    if own:
        own_loc = get_loc(handle, own, offsets, ranges, allow_zero=False)

    cam = own_loc or get_camera_from_matrix(handle, base, offsets)

    if not cam:
        for a in creatures:
            loc = get_loc(handle, a, offsets, ranges, allow_zero=False)
            if loc:
                cam = loc
                break

    if not cam:
        cam = (0.0, 0.0, 0.0)

    ps_name_cache = {}
    discovery = {}

    def get_ps_name(ps):
        if ps in ps_name_cache:
            return ps_name_cache[ps]
        best = None
        best_score = -1000000000
        best_offset = None
        offsets_to_try = []
        primary = pint(PS.get("nameOffset"))
        if primary is not None:
            offsets_to_try.append(primary)
        for cand in PS.get("nameOffsetCandidates", []):
            o = pint(cand)
            if o is not None and o not in offsets_to_try:
                offsets_to_try.append(o)
        for off in offsets_to_try:
            s = read_fstring(handle, ps, off)
            if not s:
                continue
            sc = score_name(s, args.player_name)
            if sc > best_score:
                best_score = sc
                best = s
                best_offset = hex(off)
        if best is None and not args.no_scan_player_names:
            scan_range = PS.get("nameScanRange", ["0x0", "0x1800"])
            lo = pint(scan_range[0]) if len(scan_range) > 0 else 0
            hi = pint(scan_range[1]) if len(scan_range) > 1 else 0x1800
            cands = scan_fstrings(handle, ps, lo, hi)
            for c in cands:
                s = c.get("value")
                if not s:
                    continue
                sc = score_name(s, args.player_name)
                if sc > best_score:
                    best_score = sc
                    best = s
                    best_offset = c.get("offset")
        if best is not None and ps == local_ps and best_offset is not None:
            discovery["playerStateNameOffsetUsed"] = best_offset
        ps_name_cache[ps] = best
        return best

    def class_kind(j):
        cl = A.get("classification", {})
        player_markers = cl.get(
            "playerMarkers", ["ShooterCharacter", "PlayerController", "PlayerState"]
        )
        dino_markers = cl.get(
            "dinoMarkers", ["PrimalDinoCharacter", "DinoCharacter", "PrimalCharacter"]
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

    skipped = {
        "noLocation": 0,
        "zeroLocation": 0,
        "dead": 0,
        "invalid": 0,
        "radius": 0,
    }

    for a in creatures:
        ch = chain_by_actor.get(a, [])
        j = joined_by_actor.get(a, "")

        loc = get_loc(handle, a, offsets, ranges, allow_zero=args.include_zero)
        if not loc:
            skipped["noLocation"] += 1
            continue

        if not args.include_zero and all(abs(x) < 1e-3 for x in loc):
            skipped["zeroLocation"] += 1
            continue

        dist = math.dist(loc, cam)
        if dist > args.radius:
            skipped["radius"] += 1
            continue

        kind = class_kind(j)
        is_own = (a == own)
        is_player = is_own or (kind == "player")
        label = "player" if is_player else "wild"

        blob = None
        if not is_player:
            blob = read_blob_chunked(handle, a, args.blob_size)
            owned = False
            if blob:
                for pb in ownership_packed:
                    if pb in blob:
                        owned = True
                        break
            label = "tamed" if owned else "wild"

        caps = p2.read_u64(handle, a + caps_off) if caps_off is not None else None
        hh = None
        if caps and readable_ptr(handle, caps) and hh_off is not None:
            vals = p2.read_floats(handle, caps + hh_off, 1)
            hh = vals[0] if vals else None

        hp = read_health_value(handle, a, hp_off, hp_size, hp_type)
        mhp = read_health_value(handle, a, mhp_off, hp_size, hp_type)
        level = read_level(handle, a, offsets, ranges)

        if not is_own:
            if not args.include_dead:
                if hp is not None and hp <= 0:
                    skipped["dead"] += 1
                    continue
                if mhp is not None and mhp <= 0:
                    skipped["dead"] += 1
                    continue
            if not args.include_invalid:
                if hp is None or mhp is None:
                    skipped["invalid"] += 1
                    continue
                if hh is not None and hh <= 0:
                    skipped["invalid"] += 1
                    continue

        name = None
        if is_player:
            ps = pawn_to_ps.get(a)
            if not ps and is_own:
                ps = local_ps
            if not ps:
                blob = blob or read_blob_chunked(handle, a, args.blob_size)
                if blob:
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
            name = best_actor_name(handle, idmap, a, ch, kind)

        targets.append({
            "ptr": hex(a),
            "worldX": loc[0],
            "worldY": loc[1],
            "worldZ": loc[2],
            "halfHeight": hh,
            "health": hp,
            "maxHealth": mhp,
            "level": level,
            "name": name,
            "isPlayer": is_player,
            "classLabel": label,
            "classKind": kind,
            "distance": round(dist, 1),
            "classChain": ch[:6],
        })

    targets.sort(key=lambda t: (t["distance"], t["ptr"]))

    counts = {}
    for t in targets:
        counts[t["classLabel"]] = counts.get(t["classLabel"], 0) + 1

    missing = []
    if not controller:
        missing.append("controller")
    if pint(PS.get("nameOffset")) is None and not PS.get("nameOffsetCandidates"):
        missing.append("actor.playerState.nameOffset")
    if (
        pint(CH.get("statusComponentOffset", A.get("statusComponentOffset"))) is None
        or pint(CH.get("levelOffset", A.get("levelOffset"))) is None
    ):
        missing.append("actor.character.level offsets")

    out = {
        "camera": list(cam),
        "tableSize": len(idmap),
        "counts": counts,
        "count": len(targets),
        "skipped": skipped,
        "structural_lock": {
            "ownPawn": hex(own) if own else None,
            "controller": hex(controller) if controller else None,
            "playerState": hex(local_ps) if local_ps else None,
            "method": method,
            "structuralPlayerStateOffset": structural_ps_off,
            "structuralPawnOffset": structural_pawn_off,
        },
        "missing": missing,
        "discovery": discovery,
        "targets": targets,
    }

    d = os.path.dirname(os.path.abspath(args.out))
    if d:
        os.makedirs(d, exist_ok=True)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    for t in targets[:args.limit]:
        hp = "-" if t["health"] is None else f"{t['health']:.1f}"
        mhp = "-" if t["maxHealth"] is None else f"{t['maxHealth']:.1f}"
        hh = "-" if t["halfHeight"] is None else f"{t['halfHeight']:.1f}"
        lvl = "-" if t.get("level") is None else str(t["level"])

        print(
            f"{t['distance']:>10.1f}  "
            f"{t['classLabel']:<6} "
            f"{hp:>8}/{mhp:<8} "
            f"hh={hh:>7} "
            f"lvl={lvl:>4} "
            f"{t['name']}"
        )

    print("...", json.dumps(counts))

    if out["missing"]:
        print("missing:", json.dumps(out["missing"]))

    if discovery:
        print("discovery:", json.dumps(discovery))


if __name__ == "__main__":
    main()