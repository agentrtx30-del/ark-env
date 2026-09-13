#include "process.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <cwchar>
#include <vector>
#pragma comment(lib, "psapi.lib")

bool findProcessByName(
    const wchar_t* processName,
    DWORD& pid
)
{
    pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snap, &entry))
    {
        CloseHandle(snap);
        return false;
    }
    do
    {
        if (_wcsicmp(entry.szExeFile, processName) == 0)
        {
            pid = entry.th32ProcessID;
            CloseHandle(snap);
            return true;
        }
    } while (Process32NextW(snap, &entry));
    CloseHandle(snap);
    return false;
}

bool processAlive(DWORD pid)
{
    HANDLE h = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid
    );
    if (!h)
        return false;
    bool alive = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
    CloseHandle(h);
    return alive;
}

bool findModuleBase(
    HANDLE processHandle,
    const wchar_t* moduleName,
    uint64_t& baseAddress
)
{
    baseAddress = 0;
    HMODULE modules[1024] = {};
    DWORD needed = 0;
    if (!EnumProcessModulesEx(
        processHandle,
        modules,
        sizeof(modules),
        &needed,
        LIST_MODULES_ALL))
    {
        return false;
    }
    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i)
    {
        wchar_t name[MAX_PATH] = {};
        if (!GetModuleBaseNameW(processHandle, modules[i], name, MAX_PATH))
            continue;
        if (_wcsicmp(name, moduleName) == 0)
        {
            baseAddress = reinterpret_cast<uint64_t>(modules[i]);
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// Window discovery (UWP-aware)
//-----------------------------------------------------------------------------
namespace {

struct PidWindowData
{
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

static BOOL CALLBACK enumPidWindows(HWND hwnd, LPARAM lParam)
{
    PidWindowData* d = reinterpret_cast<PidWindowData*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == d->pid && IsWindowVisible(hwnd))
    {
        d->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

struct TitleWindowData
{
    DWORD gamePid = 0;
    HWND host = nullptr;
    bool childMatch = false;
};

static BOOL CALLBACK enumChildProc(HWND hwnd, LPARAM lParam)
{
    TitleWindowData* d = reinterpret_cast<TitleWindowData*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == d->gamePid)
    {
        d->childMatch = true;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK enumTitleWindows(HWND hwnd, LPARAM lParam)
{
    TitleWindowData* d = reinterpret_cast<TitleWindowData*>(lParam);
    if (!IsWindowVisible(hwnd))
        return TRUE;
    wchar_t title[64] = {};
    if (GetWindowTextW(hwnd, title, 64) <= 0)
        return TRUE;
    // Candidate: window title contains "ARK".
    if (wcsstr(title, L"ARK") == nullptr)
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Owned directly by the game: accept.
    if (pid == d->gamePid)
    {
        d->host = hwnd;
        return FALSE;
    }
    // UWP host: accept only if the game process owns a child inside it.
    // (This rejects browsers/chat windows that merely have "ARK" in the title.)
    d->childMatch = false;
    EnumChildWindows(hwnd, enumChildProc, lParam);
    if (d->childMatch)
    {
        d->host = hwnd;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK enumExactTitleWindows(HWND hwnd, LPARAM lParam)
{
    TitleWindowData* d = reinterpret_cast<TitleWindowData*>(lParam);
    if (!IsWindowVisible(hwnd))
        return TRUE;
    wchar_t title[64] = {};
    if (GetWindowTextW(hwnd, title, 64) <= 0)
        return TRUE;
    if (wcscmp(title, L"ARK: Survival Evolved") == 0)
    {
        d->host = hwnd;
        return FALSE;
    }
    return TRUE;
}

} // anonymous namespace

bool findGameWindow(DWORD pid, HWND& hwnd)
{
    hwnd = nullptr;
    // Pass 1: top-level visible window owned by the game process.
    PidWindowData pd;
    pd.pid = pid;
    EnumWindows(enumPidWindows, reinterpret_cast<LPARAM>(&pd));
    if (pd.hwnd)
    {
        hwnd = pd.hwnd;
        return true;
    }
    // Pass 2: UWP host window, found by title, verified by child PID.
    TitleWindowData td;
    td.gamePid = pid;
    EnumWindows(enumTitleWindows, reinterpret_cast<LPARAM>(&td));
    if (td.host)
    {
        hwnd = td.host;
        return true;
    }
    // Pass 3: last resort, exact title match.
    TitleWindowData ed;
    EnumWindows(enumExactTitleWindows, reinterpret_cast<LPARAM>(&ed));
    if (ed.host)
    {
        hwnd = ed.host;
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// NEW: appx package identity (family + version) for the version drift gate
//-----------------------------------------------------------------------------
bool getPackageIdentity(DWORD pid, std::wstring& family, std::wstring& version)
{
    family.clear();
    version.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return false;
    UINT32 n = 0;
    std::wstring full;
    if (GetPackageFullName(h, &n, nullptr) == ERROR_INSUFFICIENT_BUFFER && n > 0)
    {
        std::vector<wchar_t> b(n);
        if (GetPackageFullName(h, &n, b.data()) == ERROR_SUCCESS)
            full = b.data();
    }
    CloseHandle(h);
    if (full.empty())
        return false;
    const size_t u1 = full.find(L'_');
    if (u1 == std::wstring::npos)
        return false;
    family = full.substr(0, u1);
    const size_t u2 = full.find(L'_', u1 + 1);
    if (u2 != std::wstring::npos)
        version = full.substr(u1 + 1, u2 - u1 - 1);
    return true;
}