#!/usr/bin/env python3
r"""Part 3 level probe: discover CharacterStatusComponent offset and level offset.

Usage:
python scripts/ark_part3_level_probe.py --out work/level_probe.json

If you know the level of a nearby dino/player from the UI:
python scripts/ark_part3_level_probe.py --expected-level 30 --out work/level_probe.json

If you already know the status component offset:
python scripts/ark_part3_level_probe.py --component-offset 0x7b8 --expected-level 30 --out work/level_probe.json
"""

import argparse
import json
import os
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


STATUS_MARKERS = (
    "characterstatuscomponent",
    "statuscomponent",
    "primalcharacterstatus",
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


def chain(handle, idmap, cp, depth=8, super_off=0x30):
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


def object_name(handle, idmap, obj):
    idx = (p2.read_u64(handle, obj + 0x18) or 0) & 0xFFFFFFFF
    return resolve_name(idmap, idx)


def component_looks_like_status(handle, idmap, comp):
    cp = p2.read_u64(handle, comp + 0x10)
    ch = []

    if cp and readable_ptr(handle, cp):
        ch = chain(handle, idmap, cp)

    obj = object_name(handle, idmap, comp) or ""
    text = "|".join([obj] + ch).lower()

    ok = any(marker in text for marker in STATUS_MARKERS)
    return ok, obj, ch[:4]


def scan_status_components(
    handle,
    pawn,
    idmap,
    ranges,
    start,
    end,
):
    candidates = []

    for off in range(start, end, 8):
        ptr = p2.read_u64(handle, pawn + off)
        if not ptr or not readable_ptr(handle, ptr):
            continue

        ok, obj, ch = component_looks_like_status(handle, idmap, ptr)
        if ok:
            candidates.append({
                "offset": hex(off),
                "component": hex(ptr),
                "objectName": obj,
                "classChain": ch,
            })

    return candidates


def scan_level_offsets(handle, comp, expected, start, end, max_candidates=300):
    matches = []
    candidates = []

    for off in range(start, end, 4):
        v = p2.read_i32(handle, comp + off)
        if v is None:
            continue

        # Allow 0..1000. Most ARK levels are small integers.
        if not (0 <= v <= 1000):
            continue

        item = {
            "offset": hex(off),
            "value": v,
        }

        if expected is not None:
            if v == expected:
                matches.append(item)
        else:
            if len(candidates) < max_candidates:
                candidates.append(item)

    return matches, candidates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="offsets/ark_offsets.json")
    ap.add_argument("--out", default="work/level_probe.json")
    ap.add_argument("--max-gb", type=float, default=48.0)

    ap.add_argument(
        "--expected-level",
        type=int,
        default=None,
        help="Known level of one nearby dino/player from the UI."
    )

    ap.add_argument(
        "--component-offset",
        default=None,
        help="Known pawn->CharacterStatusComponent offset, e.g. 0x7b8."
    )

    ap.add_argument(
        "--component-scan-start",
        type=lambda x: int(x, 0),
        default=0x180
    )
    ap.add_argument(
        "--component-scan-end",
        type=lambda x: int(x, 0),
        default=0x900
    )

    ap.add_argument(
        "--level-scan-start",
        type=lambda x: int(x, 0),
        default=0x0
    )
    ap.add_argument(
        "--level-scan-end",
        type=lambda x: int(x, 0),
        default=0x1000
    )

    ap.add_argument("--max-actors", type=int, default=4000)
    ap.add_argument("--max-pawns", type=int, default=100)

    args = ap.parse_args()

    handle, base, offsets, ranges = get_live(args.offsets)

    A = offsets.get("actor", {})

    regions = [m for m in iter_regions(handle) if readable(m)]
    private = [m for m in regions if m.Type in (MEM_PRIVATE, MEM_MAPPED)]
    idmap = build_name_table(handle, regions, private, args.max_gb * (1 << 30))

    world = p2.read_u64(
        handle,
        base + pint(offsets["gworld"]["rva"])
    )

    level_obj = p2.read_u64(
        handle,
        world + pint(A["actorArrayLevelOffset"])
    )

    arr = level_obj + pint(A["actorArrayOffset"])
    data = p2.read_u64(handle, arr)
    count = p2.read_i32(handle, arr + 8) or 0

    class_off = pint(A.get("uobjectHeader", {}).get("classOffset", "0x10")) or 0x10
    super_off = pint(A.get("classSuperOffset", "0x30")) or 0x30

    creatures = []

    for i in range(min(count, args.max_actors)):
        a = p2.read_u64(handle, data + i * 8)
        if not a or not p2.is_valid_ptr(handle, a, ranges):
            continue

        cp = p2.read_u64(handle, a + class_off)
        ch = chain(handle, idmap, cp, super_off=super_off)
        j = "|".join(ch)

        if "Character" in j or "Pawn" in j:
            creatures.append(a)

    if not creatures:
        sys.exit("No creature/pawn actors found.")

    known_component_offset = pint(args.component_offset)

    component_offset_counts = {}
    level_offset_counts = {}
    samples = []

    for pawn in creatures[:args.max_pawns]:
        comps = []

        if known_component_offset is not None:
            comp = p2.read_u64(handle, pawn + known_component_offset)
            if comp and readable_ptr(handle, comp):
                obj = object_name(handle, idmap, comp)
                comps.append({
                    "offset": hex(known_component_offset),
                    "component": hex(comp),
                    "objectName": obj,
                    "classChain": [],
                })
        else:
            comps = scan_status_components(
                handle,
                pawn,
                idmap,
                ranges,
                args.component_scan_start,
                args.component_scan_end,
            )

        for c in comps:
            comp_off = c["offset"]
            comp_addr = int(c["component"], 16)

            component_offset_counts[comp_off] = (
                component_offset_counts.get(comp_off, 0) + 1
            )

            matches, candidates = scan_level_offsets(
                handle,
                comp_addr,
                args.expected_level,
                args.level_scan_start,
                args.level_scan_end,
            )

            if args.expected_level is not None:
                for m in matches:
                    level_offset_counts[m["offset"]] = (
                        level_offset_counts.get(m["offset"], 0) + 1
                    )

                if matches and len(samples) < 20:
                    samples.append({
                        "pawn": hex(pawn),
                        "componentOffset": comp_off,
                        "component": c["component"],
                        "objectName": c.get("objectName"),
                        "levelMatches": matches[:10],
                    })
            else:
                if candidates and len(samples) < 10:
                    samples.append({
                        "pawn": hex(pawn),
                        "componentOffset": comp_off,
                        "component": c["component"],
                        "objectName": c.get("objectName"),
                        "levelCandidates": candidates[:40],
                    })

    recommended_component_offset = None
    if component_offset_counts:
        recommended_component_offset = max(
            component_offset_counts.items(),
            key=lambda kv: kv[1]
        )[0]

    recommended_level_offset = None
    if level_offset_counts:
        recommended_level_offset = max(
            level_offset_counts.items(),
            key=lambda kv: kv[1]
        )[0]

    out = {
        "expectedLevel": args.expected_level,
        "pawnCountScanned": min(len(creatures), args.max_pawns),
        "componentOffsetCounts": component_offset_counts,
        "levelOffsetCounts": level_offset_counts,
        "recommendedComponentOffset": recommended_component_offset,
        "recommendedLevelOffset": recommended_level_offset,
        "samples": samples,
    }

    d = os.path.dirname(os.path.abspath(args.out))
    if d:
        os.makedirs(d, exist_ok=True)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()