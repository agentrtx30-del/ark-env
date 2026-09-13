#!/usr/bin/env python3
r"""Part 3 probe5 v2: structural controller discovery + PlayerState name discovery.

Usage:
python scripts/ark_part3_probe5.py full --player-name "PluralCell8379" --out work/probe5.json
python scripts/ark_part3_probe5.py snap --out work/hp_snap.json
(take damage in game)
python scripts/ark_part3_probe5.py diff --snap work/hp_snap.json
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
    """Parse offset values that may be int or hex string."""
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
    """Walk class pointer -> super struct chain."""
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
    """Lightweight pointer sanity check independent of module ranges."""
    if not ptr or ptr & 7:
        return False
    try:
        return bool(p2.query_region(handle, ptr))
    except Exception:
        return False


def clean_text(s):
    """Return a printable cleaned string or None."""
    if not s:
        return None

    s = s.rstrip("\x00")
    if not s:
        return None

    # Prefer clean ASCII.
    if all(32 <= ord(c) < 127 for c in s):
        return s

    # Allow printable Unicode, but avoid control garbage.
    if all(c.isprintable() for c in s):
        return s

    return None


def fstrings(handle, obj, lo=0, hi=0x1800, max_len=128):
    """Scan object for UTF-16 FString fields.

    FString layout assumed:
      +0x00: wchar_t* Data
      +0x08: int32 NumChars
      +0x0c: int32 MaxChars
    """
    out = []
    if not obj:
        return out

    for off in range(lo, hi, 8):
        ptr = p2.read_u64(handle, obj + off)
        ln = p2.read_i32(handle, obj + off + 8)
        cap = p2.read_i32(handle, obj + off + 12)

        if not ptr or ptr & 7:
            continue

        if ln is None or cap is None:
            continue

        if not (0 < ln <= max_len) or cap < ln:
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

        s = clean_text(s)
        if not s:
            continue

        out.append({
            "offset": hex(off),
            "value": s,
            "ptr": hex(ptr),
            "len": ln,
            "cap": cap,
        })

    return out


GENERIC_NAMES = {
    "human",
    "player",
    "unknown",
    "none",
    "default",
    "test",
    "debug",
}


def looks_like_player_name(s):
    if not s or not (3 <= len(s) <= 64):
        return False

    if not any(c.isalpha() for c in s):
        return False

    if any(ch in s for ch in "\\/<>[]{}()=,;"):
        return False

    low = s.lower()
    if low in GENERIC_NAMES:
        return False

    if low.startswith(("tribe", "tribe of", "id_", "0x")):
        return False

    return True


def choose_name(cands, player_name=None):
    """Choose best PlayerState name candidate.

    If --player-name is supplied, exact match wins.
    Otherwise prefer unique-looking names over generic strings.
    """
    if not cands:
        return None

    exact = []
    scored = []

    for c in cands:
        v = c.get("value")
        if not v:
            continue

        if player_name and v.lower() == player_name.lower():
            exact.append(c)

        if not looks_like_player_name(v):
            continue

        score = 0

        if player_name and v.lower() == player_name.lower():
            score += 1000

        # Real ARK/player names often contain digits or mixed case.
        if any(ch.isdigit() for ch in v):
            score += 50

        if len(v) >= 6:
            score += 10

        low = v.lower()
        if low in GENERIC_NAMES:
            score -= 100

        if low.startswith("tribe"):
            score -= 100

        # Prefer earlier offsets when all else is equal.
        scored.append((score, int(c["offset"], 16), c))

    if exact:
        return exact[0]

    if scored:
        scored.sort(key=lambda x: (-x[0], x[1]))
        return scored[0][2]

    return None


def grid(handle, obj, lo, hi):
    if not obj:
        return None
    return {
        hex(o): p2.read_floats(handle, obj + o, 4)
        for o in range(lo, hi, 0x10)
    }


def get_pawn_loc(handle, pawn, offsets, ranges):
    """Read pawn location using direct offset, capsule, then root component."""
    if not pawn:
        return None

    A = offsets.get("actor", {})
    CH = A.get("character", {})

    loc_off = pint(CH.get("locationOffset", A.get("locationOffset")))
    comp_loc_off = pint(
        CH.get("componentLocationOffset", A.get("componentLocationOffset", "0xf0"))
    )
    caps_off = pint(CH.get("capsuleOffset", A.get("capsuleOffset")))
    root_off = pint(A.get("rootComponentOffset"))

    def ok(loc):
        return (
            loc
            and len(loc) == 3
            and all(
                x is not None and math.isfinite(x) and abs(x) < 1e8
                for x in loc
            )
        )

    if loc_off is not None:
        loc = p2.read_floats(handle, pawn + loc_off, 3)
        if ok(loc):
            return loc

    for comp_off in (caps_off, root_off):
        if comp_off is None:
            continue

        comp = p2.read_u64(handle, pawn + comp_off)
        if not comp or not readable_ptr(handle, comp):
            continue

        if comp_loc_off is None:
            continue

        loc = p2.read_floats(handle, comp + comp_loc_off, 3)
        if ok(loc):
            return loc

    return None


def discover_local_player(
    handle,
    base,
    offsets,
    ranges,
    controller_candidates,
    own_loc=None,
):
    """Try to find LocalPlayer -> PlayerController and viewport matrix.

    This is best-effort. Structural controller discovery still works if this fails.
    """
    try:
        engine = p2.read_u64(
            handle,
            base + pint(offsets["engineGlobal"]["rva"])
        )
        if not engine:
            return None

        gi = p2.read_u64(
            handle,
            engine + pint(offsets["gameInstanceOffset"])
        )
        if not gi:
            return None
    except Exception:
        return None

    lp_offsets = []

    primary_lp = pint(offsets.get("localPlayersOffset"))
    if primary_lp is not None:
        lp_offsets.append(primary_lp)

    alt = offsets.get("alternatives") or {}
    alt_lp = pint(alt.get("localPlayersOffset"))
    if alt_lp is not None and alt_lp not in lp_offsets:
        lp_offsets.append(alt_lp)

    cset = set(controller_candidates)
    view_off = pint(offsets.get("viewMatrixOffset", "0x60")) or 0x60
    vp_primary = pint(offsets.get("viewportClientOffset"))

    best = None

    for lp_off in lp_offsets:
        arr = p2.read_u64(handle, gi + lp_off)
        if not arr or not readable_ptr(handle, arr):
            continue

        # TArray<ULocalPlayer*> data pointer.
        lp = p2.read_u64(handle, arr)
        if not lp or not readable_ptr(handle, lp):
            continue

        controller = None
        lpco = None

        # 1. If a known localPlayerControllerOffset exists, use it.
        known_lpco = pint(offsets.get("localPlayerControllerOffset"))
        if known_lpco is not None:
            c = p2.read_u64(handle, lp + known_lpco)
            if c in cset:
                controller = c
                lpco = hex(known_lpco)

        # 2. Scan LocalPlayer blob for controller pointer.
        if controller is None and controller_candidates:
            blob = p2.read_memory(handle, lp, 0x10000)
            if blob:
                for c in controller_candidates:
                    j = blob.find(struct.pack("<Q", c))
                    if j >= 0 and j % 8 == 0:
                        controller = c
                        lpco = hex(j)
                        break

        # 3. Try viewport client / matrix.
        vc = None
        vc_off = None
        matrix = None

        if vp_primary is not None:
            v = p2.read_u64(handle, lp + vp_primary)
            if v and readable_ptr(handle, v):
                m = p2.read_floats(handle, v + view_off, 16)
                if m:
                    finite = sum(
                        1 for x in m
                        if x is not None and math.isfinite(x)
                    )
                    if finite >= 12:
                        vc = v
                        vc_off = hex(vp_primary)
                        matrix = m

        # 4. If primary viewport offset failed, scan LocalPlayer for a plausible viewport client.
        if vc is None:
            lpb = p2.read_memory(handle, lp, 0x2000) or b""
            for off in range(0, len(lpb) - 8, 8):
                v = struct.unpack_from("<Q", lpb, off)[0]
                if not v or v & 7:
                    continue

                if not readable_ptr(handle, v):
                    continue

                m = p2.read_floats(handle, v + view_off, 16)
                if not m:
                    continue

                finite = [
                    x for x in m
                    if x is not None and math.isfinite(x)
                ]
                if len(finite) < 12:
                    continue

                pos = m[12:15]
                if not all(
                    x is not None and math.isfinite(x) and abs(x) < 1e8
                    for x in pos
                ):
                    continue

                # If we know own pawn location, matrix position should be near it.
                if own_loc:
                    try:
                        if math.dist(pos, own_loc) > 10000.0:
                            continue
                    except Exception:
                        pass

                vc = v
                vc_off = hex(off)
                matrix = m
                break

        entry = {
            "localPlayersOffset": hex(lp_off),
            "localPlayer": hex(lp),
            "controller": hex(controller) if controller else None,
            "localPlayerControllerOffset": lpco,
            "viewportClient": hex(vc) if vc else None,
            "viewportClientOffset": vc_off,
            "matrix16": matrix,
        }

        if controller or matrix:
            return entry

        if best is None:
            best = entry

    return best


def cmd_full(args):
    handle, base, offsets, ranges = get_live(args.offsets)

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    A = offsets.get("actor", {})

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

    actors = []
    ps = None
    pawns = []
    controller_candidates = []

    for i in range(count):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges):
            continue

        cp = p2.read_u64(handle, a + class_off)
        ch = chain(handle, idmap, cp, super_off=super_off)
        j = "|".join(ch)

        if ps is None and ch and "PlayerState" in j:
            ps = a

        if ch and any("Character" in n for n in ch):
            pawns.append(a)

        if ch and "PlayerController" in j:
            controller_candidates.append(a)

        actors.append(a)

    controller = None
    cps_off = None
    cp_off = None
    own_pawn = None

    C = A.get("controller", {})
    known_ps_off = pint(C.get("playerStateOffset"))
    known_pawn_off = pint(C.get("pawnOffset"))
    pset = set(pawns)

    # Preferred: use known controller member offsets.
    if ps and known_ps_off is not None and known_pawn_off is not None:
        for c in controller_candidates:
            ps_read = p2.read_u64(handle, c + known_ps_off)
            pawn_read = p2.read_u64(handle, c + known_pawn_off)

            if ps_read == ps and pawn_read in pset:
                controller = c
                cps_off = hex(known_ps_off)
                cp_off = hex(known_pawn_off)
                own_pawn = pawn_read
                break

    # Fallback: structural blob scan.
    if not controller and ps:
        psb = struct.pack("<Q", ps)
        pbs = [struct.pack("<Q", p) for p in pawns]

        for a in actors:
            if a == ps or a in pset:
                continue

            blob = p2.read_memory(handle, a, 0x1000)
            if not blob or psb not in blob:
                continue

            i1 = blob.find(psb)
            if i1 % 8:
                continue

            for p, pb in zip(pawns, pbs):
                i2 = blob.find(pb)
                if i2 >= 0 and i2 % 8 == 0:
                    controller = a
                    cps_off = hex(i1)
                    cp_off = hex(i2)
                    own_pawn = p
                    break

            if controller:
                break

    # If known offsets exist but we did not get own pawn yet, try controller direct.
    if controller and own_pawn is None and known_pawn_off is not None:
        p = p2.read_u64(handle, controller + known_pawn_off)
        if p and p2.is_valid_ptr(handle, p, ranges):
            own_pawn = p

    if controller and controller not in controller_candidates:
        controller_candidates.append(controller)

    own_loc = get_pawn_loc(handle, own_pawn, offsets, ranges)

    lp_info = discover_local_player(
        handle,
        base,
        offsets,
        ranges,
        controller_candidates,
        own_loc,
    )

    lpco = None
    matrix = None

    if lp_info:
        lpco = lp_info.get("localPlayerControllerOffset")
        matrix = lp_info.get("matrix16")

        # If structural controller failed but LocalPlayer discovery found one, use it.
        if not controller and lp_info.get("controller"):
            try:
                controller = int(lp_info["controller"], 16)
            except Exception:
                pass

    ps_candidates = []
    recommended = None

    if ps:
        ps_candidates = fstrings(handle, ps, 0, args.scan_hi)
        recommended = choose_name(ps_candidates, args.player_name)

    caps = None
    root = None

    if own_pawn:
        caps_off = pint(A.get("capsuleOffset", "0x268"))
        root_off = pint(A.get("rootComponentOffset", "0x170"))

        if caps_off is not None:
            caps = p2.read_u64(handle, own_pawn + caps_off)

        if root_off is not None:
            root = p2.read_u64(handle, own_pawn + root_off)

    out = {
        "tableSize": len(idmap),
        "playerState": hex(ps) if ps else None,
        "pawnCount": len(pawns),
        "controller": hex(controller) if controller else None,
        "controllerPlayerStateOffset": cps_off,
        "controllerPawnOffset": cp_off,
        "localPlayerControllerOffset": lpco,
        "ownPawn": hex(own_pawn) if own_pawn else None,
        "ownPawnLocation": own_loc,
        "cameraFromOwnPawn": own_loc,
        "playerStateStrings": [
            {"offset": c["offset"], "value": c["value"]}
            for c in ps_candidates
        ],
        "recommendedPlayerStateNameOffset": (
            recommended["offset"] if recommended else None
        ),
        "recommendedPlayerStateNameValue": (
            recommended["value"] if recommended else None
        ),
        "playerStateNameCandidates": ps_candidates[:32],
        "localPlayer": lp_info,
        "capsuleGrid": grid(handle, caps, 0xE0, 0x150) if caps else None,
        "rootGrid": grid(handle, root, 0xE0, 0x150) if root else None,
        "matrix16": matrix,
    }

    d = os.path.dirname(os.path.abspath(args.out))
    if d:
        os.makedirs(d, exist_ok=True)

    if own_pawn:
        with open(os.path.join(d, "pawn_info.json"), "w") as f:
            json.dump({"pawn": hex(own_pawn)}, f)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    print(json.dumps(out, indent=2))


def cmd_snap(args):
    handle, base, offsets, ranges = get_live(args.offsets)

    d = os.path.dirname(os.path.abspath(args.out))
    ip = os.path.join(d, "pawn_info.json")

    if not os.path.isfile(ip):
        sys.exit("pawn_info.json missing - run 'full' first.")

    pawn = int(json.load(open(ip))["pawn"], 16)

    vals = {}
    for o in range(0x0, 0x1000, 4):
        arr = p2.read_floats(handle, pawn + o, 1)
        vals[hex(o)] = arr[0] if arr else None

    if d:
        os.makedirs(d, exist_ok=True)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"pawn": hex(pawn), "floats": vals}, f, indent=2)

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

        arr = p2.read_floats(handle, pawn + o, 1)
        new = arr[0] if arr else None

        if (
            old is not None
            and new is not None
            and math.isfinite(old)
            and math.isfinite(new)
            and 0.01 < abs(old - new) < 1000
        ):
            changed.append({
                "offset": hex(o),
                "before": old,
                "after": new,
            })

    print(json.dumps({"changed": changed}, indent=2))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name, fn in (
        ("full", cmd_full),
        ("snap", cmd_snap),
        ("diff", cmd_diff),
    ):
        p = sub.add_parser(name)
        p.add_argument("--offsets", default="offsets/ark_offsets.json")
        p.add_argument("--max-gb", type=float, default=48.0)
        p.add_argument("--out", default="work/probe5.json")
        p.add_argument("--snap", default="work/hp_snap.json")
        p.add_argument("--player-name", default=None)
        p.add_argument(
            "--scan-hi",
            type=lambda x: int(x, 0),
            default=0x1800
        )
        p.set_defaults(func=fn)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()