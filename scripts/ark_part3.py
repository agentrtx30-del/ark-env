#!/usr/bin/env python3
r"""
ARK Part 3 helper tool — singleplayer/local diagnostic only.

Subcommands:
  scan-world     Scan the live UWorld object for TArray-like actor arrays.
  dump-world     Dump raw bytes from the live UWorld object.
  inspect-array  Inspect an actor array candidate and dump first actor headers.

Example:
  python scripts/ark_part3.py scan-world --offsets offsets/ark_offsets.json --out work/world_scan.json
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_part2 as p2


def make_directories(path):
    if not path:
        return
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)


def hex_or_none(value):
    if value is None:
        return None
    if value == 0:
        return None
    return hex(value)


def get_live_world(offsets_path):
    p2.enable_debug_privilege()

    offsets = p2.load_offsets(offsets_path)
    proc, _ = p2.require_ark()

    handle = p2.open_process(
        proc["pid"],
        p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION
    )

    if not handle:
        sys.exit("OpenProcess failed. Run elevated and make sure ARK is running.")

    module_name = offsets.get("engineModule") or "ShooterGame.exe"
    mod, modules = p2.get_target_module(handle, module_name)

    base = mod["base"]
    module_ranges = [
        (m["base"], m["size"], m["name"])
        for m in modules
    ]

    gworld_rva = p2.parse_int((offsets.get("gworld") or {}).get("rva"))
    if gworld_rva is None:
        sys.exit("offsets file is missing gworld.rva")

    gworld_var = base + gworld_rva
    world = p2.read_u64(handle, gworld_var)

    if not world or not p2.is_valid_ptr(handle, world, module_ranges):
        sys.exit(json.dumps({
            "error": "world pointer invalid",
            "gworld_var": hex(gworld_var),
            "world": hex(world) if world else None
        }, indent=2))

    meta = {
        "pid": proc["pid"],
        "process_window": proc.get("window"),
        "package_family_name": proc.get("package_family_name"),
        "module": mod["name"],
        "module_base": hex(base),
        "gworld_var": hex(gworld_var),
        "world": hex(world),
    }

    return handle, world, module_ranges, meta


def scan_tarray(handle, data, module_ranges, args, where):
    candidates = []

    if len(data) < 16:
        return candidates

    last = len(data) - 16

    for off in range(0, last + 1, 8):
        ptr = struct.unpack_from("<Q", data, off)[0]
        count = struct.unpack_from("<i", data, off + 8)[0]
        capacity = struct.unpack_from("<i", data, off + 12)[0]

        if ptr == 0:
            continue

        if ptr & 7:
            continue

        if count < args.min_count:
            continue

        if count > args.max_count:
            continue

        if capacity < count:
            continue

        if capacity > args.max_capacity:
            continue

        if not p2.is_valid_ptr(handle, ptr, module_ranges):
            continue

        probe_count = min(args.probe, count)
        valid_probe = 0
        sample_actors = []

        if probe_count > 0:
            for i in range(probe_count):
                actor_ptr = p2.read_u64(handle, ptr + i * 8)
                if actor_ptr and p2.is_valid_ptr(handle, actor_ptr, module_ranges):
                    valid_probe += 1
                    if len(sample_actors) < 5:
                        sample_actors.append(hex(actor_ptr))

            required = max(1, int(probe_count * args.min_valid_ratio))
            if valid_probe < required:
                continue
        else:
            valid_probe = 0

        score = valid_probe * 100000 + min(count, 100000)

        candidates.append({
            "where": where,
            "arrayOffset": hex(off),
            "dataPointer": hex(ptr),
            "count": count,
            "capacity": capacity,
            "probeCount": probe_count,
            "validProbe": valid_probe,
            "score": score,
            "sampleActors": sample_actors,
        })

    return candidates


def cmd_scan_world(args):
    handle, world, module_ranges, meta = get_live_world(args.offsets)

    data = p2.read_memory(handle, world, args.size)
    if not data:
        sys.exit("Failed to read world object.")

    if args.save_world:
        make_directories(args.save_world)
        with open(args.save_world, "wb") as f:
            f.write(data)

    candidates = []

    # Direct scan on UWorld object.
    candidates.extend(scan_tarray(handle, data, module_ranges, args, "world"))

    # Optional one-hop deep scan:
    # Some builds put the actor array on a child object, e.g. PersistentLevel.
    if args.deep > 0:
        seen = set()
        deep_limit = min(len(data), args.deep_bytes)

        for off in range(0, deep_limit - 8, 8):
            child = struct.unpack_from("<Q", data, off)[0]

            if child == 0:
                continue

            if child & 7:
                continue

            if child == world:
                continue

            if child in seen:
                continue

            if not p2.is_valid_ptr(handle, child, module_ranges):
                continue

            seen.add(child)

            child_data = p2.read_memory(handle, child, args.child_size)
            if not child_data:
                continue

            sub_candidates = scan_tarray(
                handle,
                child_data,
                module_ranges,
                args,
                f"world+{off:#x}->child"
            )

            for c in sub_candidates:
                c["childOffset"] = hex(off)

            candidates.extend(sub_candidates)

            if args.deep_max and len(seen) >= args.deep_max:
                break

    candidates.sort(key=lambda x: x["score"], reverse=True)

    if args.limit:
        candidates = candidates[:args.limit]

    result = {
        **meta,
        "worldDumpSize": len(data),
        "candidateCount": len(candidates),
        "candidates": candidates,
    }

    if args.out:
        make_directories(args.out)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)

    print(json.dumps(result, indent=2))


def cmd_dump_world(args):
    handle, world, module_ranges, meta = get_live_world(args.offsets)

    data = p2.read_memory(handle, world, args.size)
    if not data:
        sys.exit("Failed to read world object.")

    out_path = args.out
    if out_path:
        make_directories(out_path)
        with open(out_path, "wb") as f:
            f.write(data)

    preview_bytes = min(args.preview, len(data))
    preview = p2.hexdump(data[:preview_bytes])

    result = {
        **meta,
        "size": len(data),
        "out": out_path,
        "previewBytes": preview_bytes,
        "preview": preview,
    }

    print(json.dumps(result, indent=2))


def cmd_inspect_array(args):
    handle, world, module_ranges, meta = get_live_world(args.offsets)

    base_object = world

    if args.child_offset:
        child_offset = p2.parse_int(args.child_offset)
        if child_offset is None:
            sys.exit("Invalid --child-offset")

        child = p2.read_u64(handle, world + child_offset)
        if not child or not p2.is_valid_ptr(handle, child, module_ranges):
            sys.exit("Child pointer is invalid.")

        base_object = child
        meta["childOffset"] = hex(child_offset)
        meta["child"] = hex(child)

    array_offset = p2.parse_int(args.array_offset)
    if array_offset is None:
        sys.exit("Invalid --array-offset")

    array_address = base_object + array_offset

    data_ptr = p2.read_u64(handle, array_address)
    count = p2.read_i32(handle, array_address + 8)
    capacity = p2.read_i32(handle, array_address + 12)

    if data_ptr is None or count is None or capacity is None:
        sys.exit("Failed to read actor array header.")

    if not data_ptr or not p2.is_valid_ptr(handle, data_ptr, module_ranges):
        sys.exit("Actor array data pointer is invalid.")

    if count <= 0 or capacity < count:
        sys.exit("Actor array count/capacity is invalid.")

    actors = []

    read_count = min(args.count, count)

    for i in range(read_count):
        actor_ptr = p2.read_u64(handle, data_ptr + i * 8)

        if not actor_ptr:
            continue

        if not p2.is_valid_ptr(handle, actor_ptr, module_ranges):
            continue

        header = p2.read_memory(handle, actor_ptr, args.header_bytes) or b""

        def u32_at(offset):
            if len(header) < offset + 4:
                return None
            return struct.unpack_from("<I", header, offset)[0]

        def i32_at(offset):
            if len(header) < offset + 4:
                return None
            return struct.unpack_from("<i", header, offset)[0]

        def u64_at(offset):
            if len(header) < offset + 8:
                return None
            return struct.unpack_from("<Q", header, offset)[0]

        flags_00 = u32_at(0x00)
        name_04 = i32_at(0x04)
        class_08 = u64_at(0x08)

        flags_08 = u32_at(0x08)
        name_18 = i32_at(0x18)
        class_10 = u64_at(0x10)
        outer_20 = u64_at(0x20)

        entry = {
            "index": i,
            "actor": hex(actor_ptr),
            "headerHex": p2.hexdump(header),
            "guesses": {
                "flags_0x00": flags_00,
                "name_0x04": name_04,
                "class_0x08": hex_or_none(class_08),
                "flags_0x08": flags_08,
                "name_0x18": name_18,
                "class_0x10": hex_or_none(class_10),
                "outer_0x20": hex_or_none(outer_20),
            },
            "validity": {
                "class_0x08_valid": bool(class_08 and p2.is_valid_ptr(handle, class_08, module_ranges)),
                "class_0x10_valid": bool(class_10 and p2.is_valid_ptr(handle, class_10, module_ranges)),
                "outer_0x20_valid": bool(outer_20 and p2.is_valid_ptr(handle, outer_20, module_ranges)),
            }
        }

        actors.append(entry)

    result = {
        **meta,
        "arrayAddress": hex(array_address),
        "arrayOffset": hex(array_offset),
        "dataPointer": hex(data_ptr),
        "count": count,
        "capacity": capacity,
        "actorsRead": len(actors),
        "actors": actors,
    }

    if args.out:
        make_directories(args.out)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)

    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(
        description="ARK Part 3 helper — singleplayer/local diagnostic only"
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_scan = sub.add_parser("scan-world", help="Scan UWorld for actor TArray candidates")
    p_scan.add_argument("--offsets", default="offsets/ark_offsets.json")
    p_scan.add_argument("--size", type=lambda x: int(x, 0), default=0x3000)
    p_scan.add_argument("--min-count", type=int, default=16)
    p_scan.add_argument("--max-count", type=int, default=500000)
    p_scan.add_argument("--max-capacity", type=int, default=1000000)
    p_scan.add_argument("--probe", type=int, default=24)
    p_scan.add_argument("--min-valid-ratio", type=float, default=0.5)
    p_scan.add_argument("--limit", type=int, default=50)
    p_scan.add_argument("--deep", type=int, default=0, help="0 = direct only, 1 = one-hop child scan")
    p_scan.add_argument("--deep-bytes", type=lambda x: int(x, 0), default=0x500)
    p_scan.add_argument("--deep-max", type=int, default=128)
    p_scan.add_argument("--child-size", type=lambda x: int(x, 0), default=0x2000)
    p_scan.add_argument("--save-world", default=None)
    p_scan.add_argument("--out", default=None)
    p_scan.set_defaults(func=cmd_scan_world)

    p_dump = sub.add_parser("dump-world", help="Dump raw UWorld bytes")
    p_dump.add_argument("--offsets", default="offsets/ark_offsets.json")
    p_dump.add_argument("--size", type=lambda x: int(x, 0), default=0x2000)
    p_dump.add_argument("--preview", type=int, default=256)
    p_dump.add_argument("--out", default="work/world_dump.bin")
    p_dump.set_defaults(func=cmd_dump_world)

    p_inspect = sub.add_parser("inspect-array", help="Inspect a candidate actor array")
    p_inspect.add_argument("--offsets", default="offsets/ark_offsets.json")
    p_inspect.add_argument("--array-offset", required=True)
    p_inspect.add_argument("--child-offset", default=None)
    p_inspect.add_argument("--count", type=int, default=12)
    p_inspect.add_argument("--header-bytes", type=int, default=64)
    p_inspect.add_argument("--out", default=None)
    p_inspect.set_defaults(func=cmd_inspect_array)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()