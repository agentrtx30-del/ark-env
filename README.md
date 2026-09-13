# ark-env — PART 1: Environment Discovery & Process Identification

Single-file C++/MSVC tool that, **while ARK (Store/UWP, UE4) is running on a
loaded island**, identifies the exact game process and emits a verified,
machine-readable `ark_env.json`.

## Deliverable contents

| File | Purpose |
|---|---|
| `ark_env.cpp` | The tool (C++17, Unicode, ~1200 lines, no dependencies beyond Win32) |
| `build_msvc.bat` | One-shot MSVC x64 build (auto-finds `vcvars64.bat` in any VS / Build Tools install) |
| `CMakeLists.txt` | CMake/MSVC build for IDE users |
| `ark_env.json` | The report (written to the working directory by default) |

## Build

```bat
cd ark-env
build_msvc.bat
```

Produces `ark_env.exe` (static CRT, x64, `/O2 /W4`).

## Run (prerequisites)

1. Store ARK installed, **running, on a loaded island**.
2. `ark_env.exe` run **as administrator** (UWP/AppContainer cross-process
   access requires elevation — the classic failure is `error 5`).

```bat
ark_env.exe                     -> writes .\ark_env.json
ark_env.exe --out C:\r.json     -> custom output path
ark_env.exe --pid 12345         -> force a specific PID (testing)
ark_env.exe --engine UE4-Win64-Shipping.dll --game ShooterGame-Win64-Shipping.dll
ark_env.exe --quiet             -> no console summary
```

Exit codes: `0` = game found + self-test PASS · `1` = found but self-test FAIL ·
`2` = game process not found (a valid report is still written) · `3` = fatal.

## What the report pins down (acceptance mapping)

| Requirement | JSON path |
|---|---|
| Game process name + PID, disambiguated by module set | `game_process.{name,pid,image_path,disambiguation}` |
| Engine module name / base / size / **engine version string** | `modules.engine.{name,base,size_bytes,version.version_string}` |
| Game module name / base / size / **ARK build/version string** | `modules.game.{name,base,size_bytes,version.version_string}` |
| **D3D module identity (11 vs 12)** | `modules.d3d.{flavor,name,version_string}` |
| **Game window left/top/width/height** + current resolution | `window.rect` + `display.primary_resolution` / `window` (CoreWindow matched by PID, largest area wins) |
| **Appx package family name** (via `Get-AppxPackage *ARK*`) | `appx.packages[].package_family_name` (PowerShell primary, registry `AppxManifests` fallback) |
| **Access self-test** (elevated OpenProcess + 64 KB read, result + error code) | `access_self_test.{open_process,read_memory,verdict}` — error 5 is called out explicitly |
| ASLR-safe identity | bases are recorded per-run; stable identity = names + sizes + version strings |

## How disambiguation works

1. Snapshot all processes (Toolhelp32).
2. For each, `EnumProcessModulesEx(LIST_MODULES_ALL)` + base/size
   (`GetModuleInformation`) + full path (`QueryModuleFileNameExW`).
3. A process is a **candidate** iff it loads *both* a UE engine core
   (`UE4-Win64-Shipping.dll` / `UE5-…` / `UnrealEditor-…` / `UnrealClient-…`)
   **and** a `ShooterGame-Win64-Shipping.dll` — this is what separates the
   real game from helper/overlay processes.
4. If several match, the one with D3D loaded + largest game module wins.
5. Version strings come from each module's `VS_VERSION_INFO` resource
   (`GetFileVersionInfoExW` + `VerQueryValueW`, first translation wins):
   engine → `ProductVersion`, else `FileDescription` (e.g. `UnrealEngine 4.27`);
   game → `ProductVersion`, else `FileVersion` (ARK build, e.g. `1.0.0.9234`).
6. Window: `EnumWindows` → `GetWindowThreadProcessId` == PID → largest rect.
7. Self-test: `OpenProcess(PROCESS_ALL_ACCESS)` then `ReadProcessMemory` of
   64 KiB at the engine module base; both success/failure **and the exact
   Win32 error code** are recorded (`5` = not elevated, the classic UWP fail).

## Gotchas honored

- Multiple ARK-related processes → always disambiguated by module set.
- Never hardcodes module bases (ASLR) — bases recorded per launch only.
- Store edition may ship a different exe name → the tool never relies on the
  exe name; `--engine/--game` overrides cover exotic builds.
- Regenerates cleanly after a game restart (all data re-read live each run).

## Notes

- `modules.all[]` lists every loaded module of the game process for downstream
  parts (Part 7 rendering route, hook targets, …).
- `diagnostics.module_enum_failures` records processes that refused module
  enumeration (pid + Win32 error) — useful to confirm the elevation situation.
