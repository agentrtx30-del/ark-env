// ark-esp-inject: elevated UWP loader for arkesp.dll.
// Usage: ark-esp-inject.exe [--dry] [--dll path]
//   --dry   do everything except CreateRemoteThread (report mode)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <appmodel.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

static bool isElevated()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION te{};
    DWORD sz = 0;
    bool e = false;
    if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz))
        e = (te.TokenIsElevated != 0);
    CloseHandle(tok);
    return e;
}

// Module-set disambiguation (Part 1 rule): ShooterGame.exe + StudioWildcard package.
static bool findArk(DWORD& pid, std::wstring& family)
{
    pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"ShooterGame.exe") != 0)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h)
                continue;
            UINT32 n = 0;
            std::wstring full;
            if (GetPackageFamilyName(h, &n, nullptr) == ERROR_INSUFFICIENT_BUFFER && n > 0)
            {
                std::vector<wchar_t> b(n);
                if (GetPackageFamilyName(h, &n, b.data()) == ERROR_SUCCESS)
                    full = b.data();
            }
            CloseHandle(h);
            if (full.find(L"StudioWildcard") == 0)
            {
                pid = pe.th32ProcessID;
                family = full;
                ok = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

static bool dllLoaded(HANDLE h, const wchar_t* name)
{
    HMODULE mods[2048];
    DWORD need = 0;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_ALL))
        return false;
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i)
    {
        wchar_t nm[MAX_PATH];
        if (GetModuleBaseNameW(h, mods[i], nm, MAX_PATH) && _wcsicmp(nm, name) == 0)
            return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    bool dry = false;
    std::wstring dll = L".\\arkesp.dll";
    for (int i = 1; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--dry")) dry = true;
        if (!wcscmp(argv[i], L"--dll") && i + 1 < argc) dll = argv[++i];
    }

    wprintf(L"ark-esp-inject (UWP loader)\n");
    if (!isElevated())
    {
        wprintf(L"FAIL: not elevated. Run as administrator (error 740 expected on OpenProcess otherwise).\n");
        return 2;
    }

    DWORD pid = 0;
    std::wstring family;
    if (!findArk(pid, family))
    {
        wprintf(L"FAIL: ARK (Store) process not found.\n");
        return 2;
    }
    wprintf(L"game pid=%lu package=%ls\n", pid, family.c_str());

    // Sandbox rule: stage the DLL into the app's LocalState container,
    // which the AppContainer can always read and we can always write.
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring fam = family.substr(0, family.find(L'_'));
    const std::wstring dir = std::wstring(local) + L"\\Packages\\" + fam + L"\\LocalState";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring staged = dir + L"\\arkesp.dll";
    if (!CopyFileW(dll.c_str(), staged.c_str(), FALSE))
    {
        wprintf(L"FAIL: cannot stage DLL to %ls (err %lu)\n", staged.c_str(), GetLastError());
        return 3;
    }
    wprintf(L"staged: %ls\n", staged.c_str());

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc)
    {
        wprintf(L"FAIL: OpenProcess ALL_ACCESS err=%lu (elevation/sandbox issue)\n", GetLastError());
        return 3;
    }
    wprintf(L"OpenProcess ALL_ACCESS ok\n");

    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, staged.c_str(), -1, pathA, MAX_PATH, nullptr, nullptr);
    const SIZE_T len = strlen(pathA) + 1;

    void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        wprintf(L"FAIL: VirtualAllocEx err=%lu\n", GetLastError());
        CloseHandle(proc);
        return 4;
    }
    SIZE_T wr = 0;
    if (!WriteProcessMemory(proc, remote, pathA, len, &wr))
    {
        wprintf(L"FAIL: WriteProcessMemory err=%lu\n", GetLastError());
        CloseHandle(proc);
        return 4;
    }
    wprintf(L"remote buffer at %p (%zu bytes written)\n", remote, (size_t)wr);

    if (dry)
    {
        wprintf(L"DRY RUN: stopping before CreateRemoteThread.\n");
        CloseHandle(proc);
        return 0;
    }

    // kernel32.dll is mapped at the same base in every process of this boot.
    void* ll = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)ll, remote, 0, nullptr);
    if (!th)
    {
        wprintf(L"FAIL: CreateRemoteThread err=%lu\n", GetLastError());
        CloseHandle(proc);
        return 5;
    }
    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    wprintf(L"remote LoadLibraryA exit code = 0x%lX (%s)\n", code,
            code ? "module handle = success" : "FAILURE: sandbox path problem, move the DLL");
    wprintf(L"dll in game module list: %s\n", dllLoaded(proc, L"arkesp.dll") ? "YES" : "NO");
    wprintf(L"uninject story: restart the game (arkesp.log lives next to the staged dll)\n");
    CloseHandle(proc);
    return code ? 0 : 6;
}