#!/usr/bin/env python3
r"""
ARK Part 2 helper tool.

Subcommands:
    find        Find ShooterGame.exe candidates and choose the best one.
    dump        Dump ShooterGame.exe from live process memory.
    scan        Scan a dumped module for signature candidates.
    probe       Probe a module RVA live.
    resolve     Resolve root pointer chain from offsets/ark_offsets.json.
    matrix-test Repeatedly read the resolved matrix and check whether it changes.

Run from:
    C:/Users/LLM/Llama/ark-env

Example:
    python scripts/ark_part2.py find
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import math
import os
import struct
import sys
import time

if sys.platform != "win32":
    sys.exit("This script is Windows-only.")

if ctypes.sizeof(ctypes.c_void_p) != 8:
    sys.exit("Use 64-bit Python for 64-bit ARK.")

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_ALL_ACCESS = 0x1FFFFF

TH32CS_SNAPPROCESS = 2
LIST_MODULES_ALL = 3

MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
IMAGE_SCN_MEM_WRITE = 0x80000000

TOKEN_ADJUST_PRIVILEGES = 0x0020
TOKEN_QUERY = 0x0008
SE_PRIVILEGE_ENABLED = 0x00000002

kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.GetCurrentProcess.restype = ctypes.c_void_p
kernel32.ReadProcessMemory.restype = wt.BOOL
kernel32.VirtualQueryEx.restype = ctypes.c_size_t
kernel32.GetPackageFamilyName.restype = ctypes.c_long

psapi.EnumProcessModulesEx.restype = wt.BOOL
psapi.GetModuleFileNameExW.restype = wt.DWORD
psapi.GetModuleInformation.restype = wt.BOOL

user32.EnumWindows.restype = wt.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.restype = ctypes.c_int
user32.GetClassNameW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = wt.DWORD

advapi32.OpenProcessToken.restype = wt.BOOL
advapi32.LookupPrivilegeValueW.restype = wt.BOOL
advapi32.AdjustTokenPrivileges.restype = wt.BOOL


class LUID(ctypes.Structure):
    _fields_ = [
        ("LowPart", wt.DWORD),
        ("HighPart", wt.LONG),
    ]


class LUID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Luid", LUID),
        ("Attributes", wt.DWORD),
    ]


class TOKEN_PRIVILEGES(ctypes.Structure):
    _fields_ = [
        ("PrivilegeCount", wt.DWORD),
        ("Privileges", LUID_AND_ATTRIBUTES * 1),
    ]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class MODULEINFO(ctypes.Structure):
    _fields_ = [
        ("lpBaseOfDll", ctypes.c_void_p),
        ("SizeOfImage", wt.DWORD),
        ("EntryPoint", ctypes.c_void_p),
    ]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wt.DWORD),
        ("__alignment1", wt.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
        ("__alignment2", wt.DWORD),
    ]


WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)


def enable_debug_privilege():
    advapi32.OpenProcessToken.argtypes = [
        ctypes.c_void_p,
        wt.DWORD,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    advapi32.LookupPrivilegeValueW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_wchar_p,
        ctypes.POINTER(LUID),
    ]

    advapi32.AdjustTokenPrivileges.argtypes = [
        ctypes.c_void_p,
        wt.BOOL,
        ctypes.POINTER(TOKEN_PRIVILEGES),
        wt.DWORD,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]

    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    token = ctypes.c_void_p()

    # Current process pseudo-handle.
    # Using c_void_p(-1) avoids the 64-bit overflow caused by GetCurrentProcess().
    current_process = ctypes.c_void_p(-1)

    if not advapi32.OpenProcessToken(
        current_process,
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        ctypes.byref(token),
    ):
        return False

    luid = LUID()
    if not advapi32.LookupPrivilegeValueW(
        None,
        "SeDebugPrivilege",
        ctypes.byref(luid),
    ):
        kernel32.CloseHandle(token)
        return False

    tp = TOKEN_PRIVILEGES()
    tp.PrivilegeCount = 1
    tp.Privileges[0].Luid = luid
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED

    ok = advapi32.AdjustTokenPrivileges(
        token,
        False,
        ctypes.byref(tp),
        ctypes.sizeof(tp),
        None,
        None,
    )

    kernel32.CloseHandle(token)
    return bool(ok)


def enum_processes():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return []

    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)

    processes = []

    if not kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        kernel32.CloseHandle(snap)
        return []

    while True:
        processes.append((pe.th32ProcessID, pe.szExeFile))
        if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
            break

    kernel32.CloseHandle(snap)
    return processes


def get_package_family_name(handle):
    try:
        length = wt.UINT32(0)
        kernel32.GetPackageFamilyName(handle, ctypes.byref(length), None)
        if length.value == 0:
            return None

        buf = ctypes.create_unicode_buffer(length.value)
        ret = kernel32.GetPackageFamilyName(handle, ctypes.byref(length), buf)
        if ret == 0:
            return buf.value
    except Exception:
        pass

    return None


def get_window_by_pid(pid):
    result = {}

    def enum_cb(hwnd, lparam):
        pid_out = wt.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid_out))

        if pid_out.value == pid:
            length = user32.GetWindowTextLengthW(hwnd)
            title = ""

            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buf, length + 1)
                title = buf.value

            class_buf = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, class_buf, 256)

            if title or class_buf.value:
                result.update(
                    {
                        "hwnd": int(hwnd) if hwnd else 0,
                        "title": title,
                        "class_name": class_buf.value,
                    }
                )
                return 0

        return 1

    cb = WNDENUMPROC(enum_cb)
    user32.EnumWindows(cb, 0)
    return result


def open_process(pid, access):
    return kernel32.OpenProcess(access, False, pid)


def get_modules(handle):
    modules = []
    needed = wt.DWORD(0)
    arr_size = 1024

    while True:
        arr = (ctypes.c_void_p * arr_size)()
        cb = arr_size * ctypes.sizeof(ctypes.c_void_p)

        if not psapi.EnumProcessModulesEx(handle, ctypes.byref(arr), cb, ctypes.byref(needed), LIST_MODULES_ALL):
            return []

        if needed.value <= cb:
            count = needed.value // ctypes.sizeof(ctypes.c_void_p)
            break

        arr_size = max(arr_size * 2, needed.value // ctypes.sizeof(ctypes.c_void_p) + 1)

    for i in range(count):
        hmod = arr[i]
        if not hmod:
            continue

        name_buf = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(handle, ctypes.c_void_p(hmod), name_buf, 1024)

        info = MODULEINFO()
        if not psapi.GetModuleInformation(handle, ctypes.c_void_p(hmod), ctypes.byref(info), ctypes.sizeof(info)):
            continue

        base = int(info.lpBaseOfDll or 0)
        size = int(info.SizeOfImage)
        path = name_buf.value
        name = os.path.basename(path) if path else f"module_{i}"

        modules.append(
            {
                "name": name,
                "path": path,
                "base": base,
                "base_hex": hex(base),
                "size": size,
            }
        )

    return modules


def find_ark_processes():
    candidates = []

    for pid, name in enum_processes():
        if name.lower() != "shootergame.exe":
            continue

        access = PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
        handle = open_process(pid, access)
        if not handle:
            continue

        package = get_package_family_name(handle)
        window = get_window_by_pid(pid)
        modules = get_modules(handle)
        engine_module = next((m for m in modules if m["name"].lower() == "shootergame.exe"), None)

        candidates.append(
            {
                "pid": pid,
                "name": name,
                "package_family_name": package,
                "window": window,
                "engine_module": engine_module,
                "module_count": len(modules),
            }
        )

        kernel32.CloseHandle(handle)

    def score(proc):
        s = 0

        window = proc.get("window") or {}
        title = window.get("title", "")
        class_name = window.get("class_name", "")

        if "ARK" in title:
            s += 100

        if class_name == "Windows.UI.Core.CoreWindow":
            s += 50

        package = proc.get("package_family_name") or ""
        if package.startswith("StudioWildcard"):
            s += 75

        engine_module = proc.get("engine_module")
        if engine_module:
            s += 10 + engine_module.get("size", 0) // 1_000_000

        return s

    candidates.sort(key=score, reverse=True)
    return candidates


def require_ark():
    procs = find_ark_processes()
    if not procs:
        sys.exit("No ShooterGame.exe process found. Is ARK running?")

    return procs[0], procs


def get_target_module(handle, module_name):
    modules = get_modules(handle)
    mod = next((m for m in modules if m["name"].lower() == module_name.lower()), None)

    if not mod:
        sys.exit(f"Module {module_name} not found in process.")

    return mod, modules


def read_memory(handle, address, size):
    if size <= 0:
        return b""

    buf = ctypes.create_string_buffer(size)
    read = ctypes.c_size_t(0)

    ok = kernel32.ReadProcessMemory(
        handle,
        ctypes.c_void_p(address),
        buf,
        size,
        ctypes.byref(read),
    )

    if not ok:
        return None

    return buf.raw[: read.value]


def read_u64(handle, address):
    data = read_memory(handle, address, 8)
    if not data or len(data) < 8:
        return None

    return struct.unpack("<Q", data)[0]


def read_i32(handle, address):
    data = read_memory(handle, address, 4)
    if not data or len(data) < 4:
        return None

    return struct.unpack("<i", data)[0]


def read_floats(handle, address, count):
    data = read_memory(handle, address, count * 4)
    if not data or len(data) < count * 4:
        return [None] * count

    values = struct.unpack(f"<{count}f", data[: count * 4])
    return [x if math.isfinite(x) else None for x in values]


def query_region(handle, address):
    mbi = MEMORY_BASIC_INFORMATION()
    ret = kernel32.VirtualQueryEx(
        handle,
        ctypes.c_void_p(address),
        ctypes.byref(mbi),
        ctypes.sizeof(mbi),
    )

    if ret == 0:
        return None

    return mbi


def is_valid_ptr(handle, ptr, module_ranges=None):
    if not ptr or ptr < 0x10000:
        return False

    if ptr & 7:
        return False

    mbi = query_region(handle, ptr)
    if not mbi:
        return False

    if mbi.State != MEM_COMMIT:
        return False

    if mbi.Protect & PAGE_NOACCESS:
        return False

    if mbi.Protect & PAGE_GUARD:
        return False

    if module_ranges:
        for base, size, _ in module_ranges:
            if base <= ptr < base + size:
                return False

    return True


def dump_module(handle, base, size, out_path):
    offset = 0

    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)

    with open(out_path, "wb") as out:
        while offset < size:
            address = base + offset
            mbi = query_region(handle, address)

            if not mbi:
                chunk = min(0x1000, size - offset)
                out.write(b"\x00" * chunk)
                offset += chunk
                continue

            region_end = mbi.BaseAddress + mbi.RegionSize
            chunk = min(base + size, region_end) - address

            if chunk <= 0:
                offset += 0x1000
                continue

            readable = (
                mbi.State == MEM_COMMIT
                and not (mbi.Protect & PAGE_NOACCESS)
                and not (mbi.Protect & PAGE_GUARD)
            )

            if readable:
                data = read_memory(handle, address, chunk)

                if data is None or len(data) != chunk:
                    rebuilt = bytearray()
                    pos = 0

                    while pos < chunk:
                        sub_size = min(0x1000, chunk - pos)
                        sub = read_memory(handle, address + pos, sub_size)

                        if sub and len(sub) == sub_size:
                            rebuilt.extend(sub)
                        else:
                            rebuilt.extend(b"\x00" * sub_size)

                        pos += sub_size

                    data = bytes(rebuilt)
            else:
                data = b"\x00" * chunk

            out.write(data)
            offset += chunk


def parse_sections(data):
    try:
        if len(data) < 0x40:
            return []

        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if e_lfanew + 24 > len(data):
            return []

        if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
            return []

        (
            machine,
            num_sections,
            timestamp,
            symbol_ptr,
            symbol_count,
            optional_size,
            characteristics,
        ) = struct.unpack_from("<HHIIIHH", data, e_lfanew + 4)

        optional_offset = e_lfanew + 24
        section_offset = optional_offset + optional_size

        sections = []

        for i in range(num_sections):
            off = section_offset + i * 40
            if off + 40 > len(data):
                break

            name = data[off:off + 8].rstrip(b"\x00").decode(errors="replace")
            virtual_size, virtual_address, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
            chars = struct.unpack_from("<I", data, off + 36)[0]

            sections.append(
                {
                    "name": name,
                    "rva": virtual_address,
                    "size": max(virtual_size, raw_size),
                    "chars": chars,
                }
            )

        return sections

    except Exception:
        return []


def section_for_rva(sections, rva):
    for section in sections:
        start = section["rva"]
        end = start + max(1, section["size"])

        if start <= rva < end:
            return section

    return None


def parse_pattern(pattern):
    tokens = pattern.strip().split()
    pat = bytearray()
    mask = bytearray()

    for token in tokens:
        if token in ("?", "??"):
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(token, 16))
            mask.append(1)

    return bytes(pat), bytes(mask)


def match_at(data, pos, pattern, mask):
    if pos < 0 or pos + len(pattern) > len(data):
        return False

    for i, m in enumerate(mask):
        if m and data[pos + i] != pattern[i]:
            return False

    return True


def scan_pattern(data, signature):
    pattern, mask = parse_pattern(signature["pattern"])

    if not any(mask):
        return

    start = next(i for i, m in enumerate(mask) if m)
    end = start

    while end < len(mask) and mask[end]:
        end += 1

    anchor = pattern[start:end]
    pos = 0

    while True:
        pos = data.find(anchor, pos)
        if pos < 0:
            break

        match_pos = pos - start

        if match_pos >= 0 and match_at(data, match_pos, pattern, mask):
            yield match_pos

        pos += 1


def parse_int(value):
    if value is None:
        return None

    if isinstance(value, int):
        return value

    if isinstance(value, str):
        value = value.strip()
        if not value:
            return None

        try:
            return int(value, 0)
        except Exception:
            return None

    return None


def hexdump(data):
    if not data:
        return ""

    return " ".join(f"{b:02x}" for b in data)


def load_offsets(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def cmd_find(args):
    enable_debug_privilege()
    procs = find_ark_processes()

    result = {
        "found": bool(procs),
        "chosen": procs[0] if procs else None,
        "candidates": procs,
    }

    print(json.dumps(result, indent=2, default=str))


def cmd_dump(args):
    enable_debug_privilege()

    proc, _ = require_ark()
    handle = open_process(proc["pid"], PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)

    if not handle:
        sys.exit("OpenProcess failed. Run elevated and try again.")

    mod, _ = get_target_module(handle, args.module)
    dump_module(handle, mod["base"], mod["size"], args.out)

    kernel32.CloseHandle(handle)

    print(
        json.dumps(
            {
                "dumped": args.out,
                "module": mod["name"],
                "base": mod["base_hex"],
                "size": mod["size"],
            },
            indent=2,
        )
    )


def cmd_scan(args):
    if not os.path.isfile(args.file):
        sys.exit(f"Dump file not found: {args.file}")

    if not os.path.isfile(args.sig):
        sys.exit(f"Signature file not found: {args.sig}")

    with open(args.file, "rb") as f:
        data = f.read()

    with open(args.sig, "r", encoding="utf-8") as f:
        signatures = json.load(f)

    sections = parse_sections(data)
    results = []

    for kind, signature_list in signatures.items():
        for signature in signature_list:
            count = 0

            for match in scan_pattern(data, signature):
                disp_offset = signature.get("disp_offset", 3)
                instr_match_offset = signature.get("instr_match_offset", 0)
                instr_len = signature.get("instr_len", 7)

                if match + disp_offset + 4 > len(data):
                    continue

                disp = struct.unpack_from("<i", data, match + disp_offset)[0]
                instr_start = match + instr_match_offset
                target_rva = instr_start + instr_len + disp

                if target_rva < 0 or target_rva >= len(data):
                    continue

                section = section_for_rva(sections, target_rva)
                writable = bool(section and section["chars"] & IMAGE_SCN_MEM_WRITE)

                if args.writable_only and not writable:
                    continue

                results.append(
                    {
                        "kind": kind,
                        "signature_name": signature.get("name", "unnamed"),
                        "pattern": signature.get("pattern", ""),
                        "match_rva": hex(match),
                        "global_rva": hex(target_rva),
                        "section": section["name"] if section else None,
                        "writable": writable,
                    }
                )

                count += 1
                if count >= args.limit:
                    break

    print(
        json.dumps(
            {
                "dump_file": args.file,
                "dump_size": len(data),
                "result_count": len(results),
                "results": results,
            },
            indent=2,
        )
    )


def cmd_probe(args):
    enable_debug_privilege()

    proc, _ = require_ark()
    handle = open_process(proc["pid"], PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)

    if not handle:
        sys.exit("OpenProcess failed. Run elevated and try again.")

    mod, modules = get_target_module(handle, args.module)
    rva = parse_int(args.rva)

    if rva is None:
        sys.exit("Invalid RVA.")

    address = mod["base"] + rva
    value = read_u64(handle, address)

    module_ranges = [(m["base"], m["base"] + m["size"], m["name"]) for m in modules]
    valid = is_valid_ptr(handle, value, module_ranges)

    result = {
        "pid": proc["pid"],
        "module": mod["name"],
        "module_base": mod["base_hex"],
        "rva": hex(rva),
        "address": hex(address),
        "value": hex(value) if value is not None else None,
        "value_valid": valid,
    }

    if args.bytes > 0 and value:
        target_bytes = read_memory(handle, value, args.bytes)
        result["target_bytes"] = hexdump(target_bytes) if target_bytes else None

    kernel32.CloseHandle(handle)

    print(json.dumps(result, indent=2))


def resolve_live(offsets):
    enable_debug_privilege()

    proc, _ = require_ark()
    handle = open_process(proc["pid"], PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)

    if not handle:
        sys.exit("OpenProcess failed. Run elevated and try again.")

    module_name = offsets.get("engineModule") or "ShooterGame.exe"
    mod, modules = get_target_module(handle, module_name)

    base = mod["base"]
    module_ranges = [(m["base"], m["base"] + m["size"], m["name"]) for m in modules]

    out = {
        "process_pid": proc["pid"],
        "process_window": proc.get("window"),
        "package_family_name": proc.get("package_family_name"),
        "module": mod["name"],
        "module_base": hex(base),
    }

    # GWorld
    world = None
    gworld_rva = parse_int((offsets.get("gworld") or {}).get("rva"))

    if gworld_rva is not None:
        gworld_var = base + gworld_rva
        world = read_u64(handle, gworld_var)

        out["gworld_var"] = hex(gworld_var)
        out["world"] = hex(world) if world is not None else None
        out["world_valid"] = bool(world and is_valid_ptr(handle, world, module_ranges))

    # Engine global
    engine = None
    engine_rva = parse_int((offsets.get("engineGlobal") or {}).get("rva"))

    if engine_rva is not None:
        engine_var = base + engine_rva
        engine = read_u64(handle, engine_var)

        out["engineGlobal_var"] = hex(engine_var)
        out["engine"] = hex(engine) if engine is not None else None
        out["engine_valid"] = bool(engine and is_valid_ptr(handle, engine, module_ranges))

        if not out["engine_valid"]:
            engine = None

    # GameInstance
    game_instance = None
    game_instance_offset = parse_int(offsets.get("gameInstanceOffset"))

    if engine is not None and game_instance_offset is not None:
        gi = read_u64(handle, engine + game_instance_offset)

        out["gameInstance"] = hex(gi) if gi is not None else None
        out["gameInstance_valid"] = bool(gi and is_valid_ptr(handle, gi, module_ranges))

        if out["gameInstance_valid"]:
            game_instance = gi

    # LocalPlayers TArray
    local_player = None
    local_players_offset = parse_int(offsets.get("localPlayersOffset"))

    if game_instance is not None and local_players_offset is not None:
        array_address = game_instance + local_players_offset
        data_ptr = read_u64(handle, array_address)
        count = read_i32(handle, array_address + 8)

        out["localPlayersArray"] = hex(array_address)
        out["localPlayersData"] = hex(data_ptr) if data_ptr is not None else None
        out["localPlayersCount"] = count

        if (
            data_ptr
            and count is not None
            and 0 < count <= 16
            and is_valid_ptr(handle, data_ptr, module_ranges)
        ):
            lp = read_u64(handle, data_ptr)

            if lp and is_valid_ptr(handle, lp, module_ranges):
                local_player = lp

        out["localPlayer"] = hex(local_player) if local_player else None

    # ViewportClient
    viewport_client = None
    viewport_client_offset = parse_int(offsets.get("viewportClientOffset"))

    if local_player is not None and viewport_client_offset is not None:
        vc = read_u64(handle, local_player + viewport_client_offset)

        out["viewportClient"] = hex(vc) if vc is not None else None

        if vc and is_valid_ptr(handle, vc, module_ranges):
            viewport_client = vc

    # Resolution (supports one-hop indirection via viewportOffset)
    resolution_offset = parse_int(offsets.get("resolutionOffset"))
    viewport_offset = parse_int(offsets.get("viewportOffset"))

    if viewport_client is not None and resolution_offset is not None:
        resolution_base = viewport_client

        if viewport_offset is not None:
            child = read_u64(handle, viewport_client + viewport_offset)
            if child and is_valid_ptr(handle, child, module_ranges):
                resolution_base = child

        resolution_address = resolution_base + resolution_offset
        width = read_i32(handle, resolution_address)
        height = read_i32(handle, resolution_address + 4)

        out["resolutionAddress"] = hex(resolution_address)
        out["width"] = width
        out["height"] = height
        out["resolution_valid"] = bool(
            width is not None
            and height is not None
            and 0 < width <= 16384
            and 0 < height <= 16384
        )

    # Matrix
    matrix_address = None
    view_matrix_offset = parse_int(offsets.get("viewMatrixOffset"))
    scene_view_offset = parse_int(offsets.get("sceneViewOffset"))

    if scene_view_offset is None:
        scene_view_offset = parse_int((offsets.get("optional") or {}).get("sceneViewOffset"))

    if viewport_client is not None and view_matrix_offset is not None:
        if scene_view_offset is not None:
            scene_view = read_u64(handle, viewport_client + scene_view_offset)
            out["sceneView"] = hex(scene_view) if scene_view is not None else None

            if scene_view and is_valid_ptr(handle, scene_view, module_ranges):
                matrix_address = scene_view + view_matrix_offset
        else:
            matrix_address = viewport_client + view_matrix_offset

        out["matrixAddress"] = hex(matrix_address) if matrix_address is not None else None

        if matrix_address is not None:
            out["matrixSample"] = read_floats(handle, matrix_address, 16)

    return handle, out


def cmd_resolve(args):
    offsets = load_offsets(args.offsets)
    handle, out = resolve_live(offsets)
    kernel32.CloseHandle(handle)

    print(json.dumps(out, indent=2, default=str))


def cmd_matrix_test(args):
    offsets = load_offsets(args.offsets)
    handle, out = resolve_live(offsets)

    matrix_address = out.get("matrixAddress")
    if not matrix_address:
        kernel32.CloseHandle(handle)
        sys.exit("No matrixAddress resolved. Fill viewMatrixOffset/sceneViewOffset first.")

    addr = int(matrix_address, 16) if isinstance(matrix_address, str) else int(matrix_address)

    print("Rotate/move the camera between samples.")

    previous = None
    changed_samples = 0

    for i in range(args.count):
        values = read_floats(handle, addr, 16)

        if previous is not None:
            diff = 0.0

            for a, b in zip(previous, values):
                a = a if a is not None else 0.0
                b = b if b is not None else 0.0
                diff += abs(a - b)

            if diff > args.epsilon:
                changed_samples += 1

        print(
            json.dumps(
                {
                    "sample": i,
                    "matrix": values,
                },
                indent=2,
            )
        )

        previous = values

        if i != args.count - 1:
            time.sleep(args.delay)

    print(
        json.dumps(
            {
                "samples": args.count,
                "changed_samples": changed_samples,
                "camera_coupled": changed_samples > 0,
            },
            indent=2,
        )
    )

    kernel32.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description="ARK Part 2 helper tool")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("find", help="Find ShooterGame.exe processes")
    p.set_defaults(func=cmd_find)

    p = sub.add_parser("dump", help="Dump ShooterGame.exe from live process memory")
    p.add_argument("--out", default="work/ShooterGame.dump.exe")
    p.add_argument("--module", default="ShooterGame.exe")
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("scan", help="Scan dumped module for signature candidates")
    p.add_argument("--file", default="work/ShooterGame.dump.exe")
    p.add_argument("--sig", default="scripts/signatures.json")
    p.add_argument("--limit", type=int, default=200)
    p.add_argument("--writable-only", action="store_true")
    p.set_defaults(func=cmd_scan)

    p = sub.add_parser("probe", help="Probe a module RVA live")
    p.add_argument("--rva", required=True)
    p.add_argument("--module", default="ShooterGame.exe")
    p.add_argument("--bytes", type=int, default=0)
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("resolve", help="Resolve pointer chain from ark_offsets.json")
    p.add_argument("--offsets", default="offsets/ark_offsets.json")
    p.set_defaults(func=cmd_resolve)

    p = sub.add_parser("matrix-test", help="Repeatedly read matrix and check changes")
    p.add_argument("--offsets", default="offsets/ark_offsets.json")
    p.add_argument("--count", type=int, default=5)
    p.add_argument("--delay", type=float, default=2.0)
    p.add_argument("--epsilon", type=float, default=1e-5)
    p.set_defaults(func=cmd_matrix_test)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()