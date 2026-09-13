#!/usr/bin/env python3
r"""Find localPlayerControllerOffset / reverse controller-to-localPlayer offset.

Uses addresses from work/probe5.json.

Usage:
python scripts/ark_part3_lpco_find.py --probe work/probe5.json
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ark_part2 as p2


def read_blob_chunked(handle, addr, size, chunk=0x1000):
    """Read memory in chunks because large single reads may fail."""
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


def find_aligned(blob, needle, limit=32):
    """Find all 8-byte-aligned occurrences of needle."""
    out = []
    i = 0

    while True:
        j = blob.find(needle, i)
        if j < 0:
            break

        if j % 8 == 0:
            out.append(hex(j))

        if len(out) >= limit:
            break

        i = j + 8

    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", default="work/probe5.json")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x20000)
    args = ap.parse_args()

    if not os.path.isfile(args.probe):
        sys.exit(f"missing probe file: {args.probe}")

    probe = json.load(open(args.probe, encoding="utf-8"))

    lp_hex = (probe.get("localPlayer") or {}).get("localPlayer")
    controller_hex = probe.get("controller")

    if not lp_hex:
        sys.exit("probe5.json does not contain localPlayer.localPlayer")

    if not controller_hex:
        sys.exit("probe5.json does not contain controller")

    lp = int(lp_hex, 16)
    controller = int(controller_hex, 16)

    p2.enable_debug_privilege()
    proc, _ = p2.require_ark()
    handle = p2.open_process(
        proc["pid"],
        p2.PROCESS_VM_READ | p2.PROCESS_QUERY_INFORMATION
    )

    lp_blob = read_blob_chunked(handle, lp, args.size)
    ctrl_blob = read_blob_chunked(handle, controller, args.size)

    controller_needle = struct.pack("<Q", controller)
    lp_needle = struct.pack("<Q", lp)

    result = {
        "localPlayer": hex(lp),
        "controller": hex(controller),
        "localPlayerBlobSize": len(lp_blob),
        "controllerBlobSize": len(ctrl_blob),
        "localPlayerControllerOffsetCandidates": find_aligned(
            lp_blob,
            controller_needle
        ),
        "controllerToLocalPlayerOffsetCandidates": find_aligned(
            ctrl_blob,
            lp_needle
        ),
    }

    if probe.get("ownPawn"):
        pawn = int(probe["ownPawn"], 16)
        result["controllerToOwnPawnOffsetCandidates"] = find_aligned(
            ctrl_blob,
            struct.pack("<Q", pawn)
        )

    if probe.get("playerState"):
        ps = int(probe["playerState"], 16)
        result["controllerToPlayerStateOffsetCandidates"] = find_aligned(
            ctrl_blob,
            struct.pack("<Q", ps)
        )

    print(json.dumps(result, indent=2))

    lpco_candidates = result["localPlayerControllerOffsetCandidates"]
    ctrl_to_lp_candidates = result["controllerToLocalPlayerOffsetCandidates"]

    if lpco_candidates:
        print()
        print("Recommended offset:")
        print(f'  "localPlayerControllerOffset": "{lpco_candidates[0]}"')
    elif ctrl_to_lp_candidates:
        print()
        print("Did not find LocalPlayer -> Controller.")
        print("Found reverse Controller -> LocalPlayer instead:")
        print(f'  "controller.localPlayerOffset": "{ctrl_to_lp_candidates[0]}"')
        print()
        print("Your current target dump can still work without this.")
    else:
        print()
        print("No direct LocalPlayer <-> Controller pointer found.")
        print("This is okay because structural controller discovery already works.")


if __name__ == "__main__":
    main()