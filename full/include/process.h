#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

bool findProcessByName(
    const wchar_t* processName,
    DWORD& pid
);

bool processAlive(DWORD pid);

bool findModuleBase(
    HANDLE processHandle,
    const wchar_t* moduleName,
    uint64_t& baseAddress
);

// UWP-aware: finds the visible ARK window even when the top-level
// window is owned by ApplicationFrameHost.exe.
bool findGameWindow(DWORD pid, HWND& hwnd);

// NEW: reads the appx package family + version ("1.212.962.2") for a pid.
// Returns false when the process is unpackaged or the query fails.
bool getPackageIdentity(DWORD pid, std::wstring& family, std::wstring& version);