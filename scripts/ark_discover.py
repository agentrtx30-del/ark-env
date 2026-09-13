#!/usr/bin/env python3
r"""
ARK Part 2 member-offset discovery helper (v3).

Commands:
    winsize     Print the game's real window client size.
    chain       Hunt gameInstance/localPlayers/viewportClient/resolution offsets.
    matrixhunt  Hunt the 16-float view matrix under the viewport client.

Run from C:/Users/LLM/Llama/ark-env:
    python scripts/ark_discover.py winsize
    python scripts/ark_discover.py chain --offsets offsets/ark_offsets.json
    python scripts/ark_discover.py matrixhunt --offsets offsets/ark_offsets.json
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import math
import struct
import sys
import time

import ark_part2 as A


def qwords(buf):
    for off in range(0, len(buf) - 8, 8):
        yield off, struct.unpack_from("<Q", buf, off)[0]


def open_game(offsets):
    A.enable_debug_privilege()
    proc, _ = A.require_ark()
    handle = A.open_process(proc["pid"], A.PROCESS_VM_READ | A.PROCESS_QUERY_INFORMATION)
    if not handle:
        sys.exit("OpenProcess failed. Run elevated.")
    mod, modules = A.get_target_module(handle, offsets.get("engineModule") or "ShooterGame.exe")
    ranges = [(m["base"], m["base"] + m["size"], m["name"]) for m in modules]
    return handle, mod["base"], ranges, proc


def get_engine(handle, offsets, base, ranges):
    rva = A.parse_int((offsets.get("engineGlobal") or {}).get("rva"))
    if rva is None:
        sys.exit("engineGlobal.rva is missing in ark_offsets.json")
    engine = A.read_u64(handle, base + rva)
    if not engine or not A.is_valid_ptr(handle, engine, ranges):
        sys.exit("engine pointer invalid")
    return engine


def enum_game_windows(pid):
    wins = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd, lparam):
        pid_out = wt.DWORD(0)
        A.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid_out))
        if pid_out.value == pid:
            cls = ctypes.create_unicode_buffer(256)
            A.user32.GetClassNameW(hwnd, cls, 256)
            title = ctypes.create_unicode_buffer(256)
            A.user32.GetWindowTextW(hwnd, title, 256)
            rect = wt.RECT()
            A.user32.GetClientRect(hwnd, ctypes.byref(rect))
            wins.append({
                "hwnd": int(hwnd) if hwnd else 0,
                "class": cls.value,
                "title": title.value,
                "client_w": rect.right - rect.left,
                "client_h": rect.bottom - rect.top,
            })
        return 1

    cbref = WNDENUMPROC(cb)
    A.user32.EnumWindows(cbref, 0)
    return wins


def pick_game_window(wins):
    for w in wins:
        if w["class"] == "Windows.UI.Core.CoreWindow" and w["client_w"] > 0:
            return w
    for w in wins:
        if "ARK" in w["title"] and w["client_w"] > 0:
            return w
    for w in wins:
        if w["client_w"] > 0:
            return w
    return None


def cmd_winsize(args):
    A.enable_debug_privilege()
    proc, _ = A.require_ark()
    wins = enum_game_windows(proc["pid"])
    picked = pick_game_window(wins)
    print(json.dumps({"pid": proc["pid"], "windows": wins, "picked": picked}, indent=2))


def scan_sizes(buf, width, height):
    exact = []
    plausible = []
    for ro in range(0, len(buf) - 8, 4):
        w, h = struct.unpack_from("<ii", buf, ro)
        if w == width and h == height:
            exact.append(ro)
        elif ro % 8 == 0 and 320 <= w <= 8192 and 240 <= h <= 8192 and len(plausible) < 8:
            plausible.append((ro, w, h))
    return exact, plausible


def ptr_map(buf):
    out = {}
    for off, val in qwords(buf):
        if val and (val & 7) == 0 and val not in out:
            out[val] = off
    return out


def cmd_chain(args):
    offsets = A.load_offsets(args.offsets)
    handle, base, ranges, proc = open_game(offsets)
    engine = get_engine(handle, offsets, base, ranges)

    width, height = args.width, args.height
    window_info = None
    if width <= 0 or height <= 0:
        wins = enum_game_windows(proc["pid"])
        picked = pick_game_window(wins)
        if not picked:
            sys.exit("could not detect game window size; pass --width/--height")
        width, height = picked["client_w"], picked["client_h"]
        window_info = picked

    engine_buf = A.read_memory(handle, engine, args.engine_size)
    if not engine_buf:
        sys.exit("could not read engine object")

    engine_ptrs = ptr_map(engine_buf)
    results = []

    for o, gi in qwords(engine_buf):
        if not A.is_valid_ptr(handle, gi, ranges):
            continue
        gi_buf = A.read_memory(handle, gi, args.gi_size)
        if not gi_buf:
            continue

        gi_ptrs = ptr_map(gi_buf)

        for lo, data in qwords(gi_buf):
            if lo + 16 > len(gi_buf):
                continue
            count, cap = struct.unpack_from("<iI", gi_buf, lo + 8)
            if count != 1 or not (1 <= cap <= 16):
                continue
            if not A.is_valid_ptr(handle, data, ranges):
                continue

            lp = A.read_u64(handle, data)
            if not A.is_valid_ptr(handle, lp, ranges):
                continue
            lp_buf = A.read_memory(handle, lp, args.lp_size)
            if not lp_buf:
                continue

            seen_vtables = set()

            for vo, vc in qwords(lp_buf):
                if not A.is_valid_ptr(handle, vc, ranges):
                    continue
                vc_buf = A.read_memory(handle, vc, args.vc_size)
                if not vc_buf or len(vc_buf) < 8:
                    continue

                vtable = struct.unpack_from("<Q", vc_buf, 0)[0]
                if vtable in seen_vtables:
                    continue
                seen_vtables.add(vtable)

                score = 0

                engine_anchor = engine_ptrs.get(vc)
                if engine_anchor is not None:
                    score += 100

                gi_anchor = gi_ptrs.get(vc)
                if gi_anchor is not None:
                    score += 50

                exact_d, plausible_d = scan_sizes(vc_buf, width, height)
                score += 40 * len(exact_d)
                if args.allow_plausible:
                    score += min(4, len(plausible_d))

                exact_1 = []
                plausible_1 = []
                slots = 0
                for s, p in qwords(vc_buf[:0x300]):
                    if slots >= args.slots:
                        break
                    if not A.is_valid_ptr(handle, p, ranges):
                        continue
                    slots += 1
                    p_buf = A.read_memory(handle, p, 0x2000)
                    if not p_buf:
                        continue
                    ex, pl = scan_sizes(p_buf, width, height)
                    for ro in ex:
                        exact_1.append({"slot": hex(s), "off": hex(ro)})
                    if not plausible_1:
                        plausible_1 = [
                            {"slot": hex(s), "off": hex(x[0]), "w": x[1], "h": x[2]}
                            for x in pl[:4]
                        ]
                    score += 30 * len(ex)

                if score == 0:
                    continue

                results.append({
                    "score": score,
                    "gameInstanceOffset": hex(o),
                    "localPlayersOffset": hex(lo),
                    "localPlayer": hex(lp),
                    "viewportClientOffset": hex(vo),
                    "viewportClient": hex(vc),
                    "vc_vtable": hex(vtable),
                    "engine_anchor_offset": hex(engine_anchor) if engine_anchor is not None else None,
                    "gi_anchor_offset": hex(gi_anchor) if gi_anchor is not None else None,
                    "resolution_exact_direct": [hex(x) for x in exact_d],
                    "resolution_exact_via_slot": exact_1,
                    "resolution_plausible_direct": [
                        {"off": hex(x[0]), "w": x[1], "h": x[2]} for x in plausible_d[:6]
                    ],
                    "resolution_plausible_via_slot": plausible_1[:6],
                })

                if len(results) >= 200:
                    break
            if len(results) >= 200:
                break
        if len(results) >= 200:
            break

    results.sort(key=lambda r: r["score"], reverse=True)
    results = results[: args.max_results]

    A.kernel32.CloseHandle(handle)
    print(json.dumps({
        "engine": hex(engine),
        "used_resolution": [width, height],
        "window": window_info,
        "result_count": len(results),
        "results": results,
    }, indent=2))


def looks_matrix(vals):
    if not all(math.isfinite(v) for v in vals):
        return False
    if abs(vals[0]) < 1e-6 and abs(vals[5]) < 1e-6:
        return False
    return True


def cmd_matrixhunt(args):
    offsets = A.load_offsets(args.offsets)
    handle, base, ranges, proc = open_game(offsets)

    gi_off = A.parse_int(offsets.get("gameInstanceOffset"))
    lp_off = A.parse_int(offsets.get("localPlayersOffset"))
    vc_off = A.parse_int(offsets.get("viewportClientOffset"))
    if None in (gi_off, lp_off, vc_off):
        sys.exit("fill gameInstanceOffset/localPlayersOffset/viewportClientOffset first (run chain)")

    engine = get_engine(handle, offsets, base, ranges)
    gi = A.read_u64(handle, engine + gi_off)
    data = A.read_u64(handle, gi + lp_off)
    lp = A.read_u64(handle, data)
    vc = A.read_u64(handle, lp + vc_off)
    if not vc:
        sys.exit("chain broken: viewportClient is null")

    regions = [("direct", vc)]
    vc_head = A.read_memory(handle, vc, 0x400) or b""
    for off, p in qwords(vc_head):
        if A.is_valid_ptr(handle, p, ranges):
            regions.append((hex(off), p))
            if len(regions) > args.max_ptrs:
                break

    def sample():
        out = []
        for name, addr in regions:
            buf = A.read_memory(handle, addr, args.size) or b""
            out.append((name, addr, buf))
        return out

    s1 = sample()
    print(f"SAMPLE 1 taken. NOW ROTATE / MOVE THE CAMERA for {args.delay}s ...", flush=True)
    time.sleep(args.delay)
    s2 = sample()
    print("SAMPLE 2 taken.", flush=True)

    candidates = []
    for (name1, addr1, buf1), (name2, addr2, buf2) in zip(s1, s2):
        if name1 != name2 or addr1 != addr2:
            continue
        for off in range(0, min(len(buf1), len(buf2)) - 64, 16):
            v1 = struct.unpack_from("<16f", buf1, off)
            v2 = struct.unpack_from("<16f", buf2, off)
            if not looks_matrix(v1) or not looks_matrix(v2):
                continue
            changed = sum(1 for a, b in zip(v1, v2) if abs(a - b) > 1e-6)
            if changed >= args.min_changed:
                candidates.append({
                    "region": name1,
                    "offset_in_region": hex(off),
                    "address": hex(addr1 + off),
                    "changed_floats": changed,
                    "sample1": [round(x, 5) for x in v1],
                    "sample2": [round(x, 5) for x in v2],
                })
                if len(candidates) >= args.max_results:
                    break
        if len(candidates) >= args.max_results:
            break

    A.kernel32.CloseHandle(handle)
    print(json.dumps({
        "viewportClient": hex(vc),
        "candidate_count": len(candidates),
        "candidates": candidates,
    }, indent=2))


def main():
    parser = argparse.ArgumentParser(description="ARK member-offset discovery (v3)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("winsize")
    p.set_defaults(func=cmd_winsize)

    p = sub.add_parser("chain")
    p.add_argument("--offsets", default="offsets/ark_offsets.json")
    p.add_argument("--width", type=int, default=0)
    p.add_argument("--height", type=int, default=0)
    p.add_argument("--engine-size", type=lambda x: int(x, 0), default="0x8000")
    p.add_argument("--gi-size", type=lambda x: int(x, 0), default="0x400")
    p.add_argument("--lp-size", type=lambda x: int(x, 0), default="0x400")
    p.add_argument("--vc-size", type=lambda x: int(x, 0), default="0x4000")
    p.add_argument("--slots", type=int, default=12)
    p.add_argument("--allow-plausible", action="store_true")
    p.add_argument("--max-results", type=int, default=20)
    p.set_defaults(func=cmd_chain)

    p = sub.add_parser("matrixhunt")
    p.add_argument("--offsets", default="offsets/ark_offsets.json")
    p.add_argument("--size", type=lambda x: int(x, 0), default="0x4000")
    p.add_argument("--max-ptrs", type=int, default=12)
    p.add_argument("--delay", type=float, default=3.0)
    p.add_argument("--min-changed", type=int, default=6)
    p.add_argument("--max-results", type=int, default=20)
    p.set_defaults(func=cmd_matrixhunt)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()