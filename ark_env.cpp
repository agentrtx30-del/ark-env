// ============================================================================
//  ark-env — PART 1: Environment Discovery & Process Identification  (v1.1.0)
//  Target: Windows 10/11 x64, MSVC (C++17), Unicode
// ============================================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winver.h>
#include <appmodel.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace ark {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)towlower((unsigned short)c); });
    return s;
}

static std::string JEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b";  break;
        case '\f': o += "\\f";  break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20 || c > 0x7E) {          // escape ALL control + non-ASCII bytes
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    return o;
}

static std::string JEsc(const std::wstring& w) { return JEsc(ToUtf8(w)); }

static std::string JHex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

static std::string JHexPtr(UINT_PTR v) { return JHex((uint64_t)v); }

static std::wstring FormatVersion(unsigned ms, unsigned ls) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF);
    return ToWide(b);
}

// ---------------------------------------------------------------------------
// OS info / elevation / DPI
// ---------------------------------------------------------------------------
struct OsVersion { unsigned major = 0, minor = 0, build = 0; bool ok = false; };

static OsVersion GetOsVersion() {
    OsVersion v;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    typedef long (WINAPI *RtlGetVersionFn)(void*);
    auto fn = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    if (!fn) return v;
    struct RtlOsVer {
        unsigned dwOSVersionInfoVersion, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
        wchar_t szCSDVersion[128];
    } info{};
    if (fn(&info) == 0) {
        v.ok = true; v.major = info.dwMajorVersion; v.minor = info.dwMinorVersion; v.build = info.dwBuildNumber;
    }
    return v;
}

static std::string OsString(const OsVersion& v) {
    std::ostringstream o;
    if (!v.ok) return "Windows (unknown build)";
    o << "Windows " << (v.major == 10 ? (v.build >= 22000 ? "11" : "10") : std::to_string(v.major))
      << " (build " << v.build << ", " << v.major << "." << v.minor << "." << v.build << ")";
    return o.str();
}

static std::string UtcNowIso8601() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER q; q.LowPart = ft.dwLowDateTime; q.HighPart = ft.dwHighDateTime;
    __int64 sec = q.QuadPart / 10000000;
    __int64 days = sec / 86400, rem = sec % 86400;
    int hh = (int)(rem / 3600), mm = (int)((rem % 3600) / 60), ss = (int)(rem % 60);
    __int64 z = days + 719468;
    __int64 era = (z >= 0 ? z : z - 146096) / 146097;
    __int64 doe = z - era * 146097;
    __int64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    __int64 y = yoe + era * 400;
    __int64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    __int64 mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += (m <= 2) ? 1 : 0;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)y, m, d, hh, mm, ss);
    return buf;
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    bool elevated = false;
    TOKEN_ELEVATION te{};
    DWORD sz = 0;
    if (GetTokenInformation(tok, TokenElevation, reinterpret_cast<LPVOID>(&te), sizeof te, &sz))
        elevated = te.TokenIsElevated != 0;
    CloseHandle(tok);
    return elevated;
}

static bool EnableDebugPrivilege(DWORD& err) {
    err = 0;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        err = GetLastError(); return false;
    }
    LUID luid{};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, nullptr, nullptr);
        err = GetLastError();
        ok = (err == ERROR_SUCCESS);
    } else {
        err = GetLastError();
    }
    CloseHandle(tok);
    return ok;
}

static void SetDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetCtxFn)(DPI_AWARENESS_CONTEXT);
    auto setCtx = u32 ? (SetCtxFn)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
    if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr) return;
    SetProcessDPIAware();
}

static UINT GetWindowDpi(HWND h) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *DpiFn)(HWND);
    auto fn = u32 ? (DpiFn)(void*)GetProcAddress(u32, "GetDpiForWindow") : nullptr;
    return fn ? fn(h) : 96;
}

// ---------------------------------------------------------------------------
// Process & module enumeration
// ---------------------------------------------------------------------------
struct ModuleInfo {
    std::wstring name;
    std::wstring path;
    uint64_t base = 0;
    uint64_t size = 0;
};

struct ProcEntry { DWORD pid = 0; std::wstring name; };

static bool EnumAllProcesses(std::vector<ProcEntry>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (!Process32FirstW(snap, &pe)) ok = false;
    else {
        do {
            if (pe.th32ProcessID == 0) continue;
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.name = pe.szExeFile;
            out.push_back(std::move(e));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

static std::wstring GetProcessImage(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::wstring();
    wchar_t buf[4096] = {};
    DWORD n = (DWORD)(sizeof(buf) / sizeof(wchar_t));
    std::wstring p;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) p = buf;
    CloseHandle(h);
    return p;
}

static bool ListProcessModules(DWORD pid, std::vector<ModuleInfo>& out, DWORD& winErr) {
    winErr = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { winErr = GetLastError(); return false; }
    DWORD need = 0;
    if (!EnumProcessModulesEx(h, nullptr, 0, &need, LIST_MODULES_ALL)) {
        winErr = GetLastError(); CloseHandle(h); return false;
    }
    size_t count = need / sizeof(HMODULE);
    if (count == 0) { CloseHandle(h); return true; }
    std::vector<HMODULE> mods(count);
    if (!EnumProcessModulesEx(h, mods.data(), (DWORD)(count * sizeof(HMODULE)), &need, LIST_MODULES_ALL)) {
        winErr = GetLastError(); CloseHandle(h); return false;
    }
    for (HMODULE m : mods) {
        ModuleInfo mi;
        wchar_t nameBuf[1024] = {};
        if (GetModuleBaseNameW(h, m, nameBuf, (DWORD)(sizeof(nameBuf) / sizeof(wchar_t))))
            mi.name = nameBuf;
        wchar_t pathBuf[4096] = {};
        DWORD pn = (DWORD)(sizeof(pathBuf) / sizeof(wchar_t));
        if (GetModuleFileNameExW(h, m, pathBuf, pn)) mi.path = pathBuf;
        MODULEINFO mi64{};
        if (GetModuleInformation(h, m, &mi64, sizeof mi64)) {
            mi.base = (uint64_t)(uintptr_t)mi64.lpBaseOfDll;
            mi.size = mi64.SizeOfImage;
        }
        if (mi.name.empty()) {
            size_t slash = mi.path.find_last_of(L"\\/");
            mi.name = (slash == std::wstring::npos) ? mi.path : mi.path.substr(slash + 1);
        }
        if (!mi.name.empty()) out.push_back(std::move(mi));
    }
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------------------
// Module identity matchers
// ---------------------------------------------------------------------------
static bool HasSuffix(const std::wstring& s, const wchar_t* suf) {
    size_t n = std::wcslen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static bool IsEngineDll(const std::wstring& n) {
    std::wstring s = ToLower(n);
    return s == L"ue4-win64-shipping.dll" ||
           s == L"ue5-win64-shipping.dll" ||
           s == L"unrealeditor-win64-shipping.dll" ||
           s == L"unrealclient-win64-shipping.dll";
}

static bool IsGameDll(const std::wstring& n) {
    std::wstring s = ToLower(n);
    return s.rfind(L"shootergame", 0) == 0 && HasSuffix(s, L"-win64-shipping.dll");
}

static bool IsD3D11(const std::wstring& n) { return ToLower(n) == L"d3d11.dll"; }
static bool IsD3D12(const std::wstring& n) { return ToLower(n) == L"d3d12.dll"; }

// ---------------------------------------------------------------------------
// Version resources
// ---------------------------------------------------------------------------
struct VersionInfo {
    bool ok = false;
    unsigned fver_ms = 0, fver_ls = 0, pver_ms = 0, pver_ls = 0;
    std::map<std::wstring, std::wstring> strings;
    std::wstring error;
    std::wstring via;
};

static void ParseVerNode(const BYTE* d, size_t off, size_t end, VersionInfo& v, int depth) {
    if (depth > 8 || off + 6 > end) return;
    auto u16 = [&](size_t o) { uint16_t x = 0; std::memcpy(&x, d + o, 2); return x; };
    uint16_t len = u16(off), valLen = u16(off + 2), type = u16(off + 4);
    if (len < 6 || off + len > end) return;
    size_t nodeEnd = off + len;
    size_t k = off + 6, ke = k;
    while (ke + 1 < nodeEnd && u16(ke) != 0) ke += 2;
    std::wstring key(reinterpret_cast<const wchar_t*>(d + k), (ke - k) / 2);
    size_t val = ((ke + 2) + 3) & ~size_t(3);
    size_t valBytes = (type == 1) ? (size_t)valLen * 2 : (size_t)valLen;
    if (val + valBytes > nodeEnd) valBytes = (val < nodeEnd) ? nodeEnd - val : 0;
    if (type == 0 && valBytes >= sizeof(VS_FIXEDFILEINFO)) {
        VS_FIXEDFILEINFO f{};
        std::memcpy(&f, d + val, sizeof f);
        if (f.dwSignature == 0xFEEF04BD) {
            v.fver_ms = f.dwFileVersionMS; v.fver_ls = f.dwFileVersionLS;
            v.pver_ms = f.dwProductVersionMS; v.pver_ls = f.dwProductVersionLS;
        }
    } else if (type == 1 && valBytes >= 2 && !key.empty()) {
        size_t chars = valBytes / 2;
        const wchar_t* p = reinterpret_cast<const wchar_t*>(d + val);
        while (chars && p[chars - 1] == L'\0') --chars;
        if (chars && v.strings.find(key) == v.strings.end())
            v.strings[key] = std::wstring(p, chars);
        return;
    }
    size_t child = (val + valBytes + 3) & ~size_t(3);
    while (child + 6 <= nodeEnd) {
        uint16_t clen = u16(child);
        if (clen < 6 || child + clen > nodeEnd) break;
        ParseVerNode(d, child, child + clen, v, depth + 1);
        child += clen;
    }
}

static VersionInfo ReadFileVersionInfo(const std::wstring& path) {
    VersionInfo v;
    v.via = L"none";
    if (path.empty()) { v.error = L"no file path available"; return v; }
    DWORD sz = 0;
    if (!GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &sz) || sz == 0) {
        DWORD e = GetLastError();
        v.error = L"GetFileVersionInfoSizeExW failed (error " + std::to_wstring(e) + L": " +
                  (e == 1812 ? L"no version resource found"
                             : (e == ERROR_ACCESS_DENIED ? L"access denied (WindowsApps ACL)"
                                                         : L"see win32 error")) + L")";
        return v;
    }
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, sz, buf.data())) {
        v.error = L"GetFileVersionInfoExW failed (error " + std::to_wstring(GetLastError()) + L")";
        return v;
    }
    LPVOID pv = nullptr;
    UINT plen = 0;
    if (VerQueryValueW(buf.data(), L"\\", &pv, &plen) && pv && plen >= sizeof(VS_FIXEDFILEINFO)) {
        auto* fvi = reinterpret_cast<const VS_FIXEDFILEINFO*>(pv);
        v.fver_ms = fvi->dwFileVersionMS; v.fver_ls = fvi->dwFileVersionLS;
        v.pver_ms = fvi->dwProductVersionMS; v.pver_ls = fvi->dwProductVersionLS;
    }
    LPVOID trp = nullptr;
    UINT trn = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", &trp, &trn) && trp && trn >= 4) {
        auto* tr = reinterpret_cast<const WORD*>(trp);
        for (UINT i = 0; i + 1 < trn; i += 2) {
            char keybuf[96];
            std::snprintf(keybuf, sizeof keybuf, "\\%04x,%04x\\", tr[i], tr[i + 1]);
            std::wstring key = ToWide(keybuf);
            static const wchar_t* const kNames[] = {
                L"CompanyName", L"FileDescription", L"FileVersion", L"InternalName",
                L"LegalCopyright", L"LegalTrademarks", L"OriginalFilename",
                L"PrivateBuild", L"ProductName", L"ProductVersion"
            };
            for (const wchar_t* nm : kNames) {
                std::wstring full = key + nm;
                LPVOID sv = nullptr;
                UINT sl = 0;
                if (VerQueryValueW(buf.data(), full.c_str(), &sv, &sl) && sv && sl >= 2) {
                    std::wstring val(reinterpret_cast<const wchar_t*>(sv), sl / sizeof(wchar_t));
                    if (!val.empty() && v.strings.find(nm) == v.strings.end())
                        v.strings[nm] = std::move(val);
                }
            }
        }
    }
    v.ok = v.fver_ms || v.pver_ms || !v.strings.empty();
    if (v.ok) v.via = L"file";
    return v;
}

static VersionInfo ReadVersionFromMemory(DWORD pid, uint64_t base) {
    VersionInfo v;
    v.via = L"none";
    if (base == 0) { v.error = L"no module base known"; return v; }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { v.error = L"OpenProcess(VM_READ) failed (error " + std::to_wstring(GetLastError()) + L")"; return v; }
    auto ReadAt = [&](uint64_t addr, size_t n, std::vector<BYTE>& out) -> bool {
        out.assign(n, 0);
        SIZE_T got = 0;
        return ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), out.data(), (DWORD)n, &got) && got == n;
    };
    auto Fail = [&](const char* why) {
        v.error = L"memory version walk failed: " + ToWide(std::string(why));
        CloseHandle(h);
        return v;
    };
    std::vector<BYTE> b;
    if (!ReadAt(base, sizeof(IMAGE_DOS_HEADER), b)) return Fail("cannot read DOS header");
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return Fail("bad DOS magic");
    if (!ReadAt(base + (uint64_t)dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64), b))
        return Fail("cannot read PE header");
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b.data());
    if (nt->Signature != IMAGE_NT_SIGNATURE) return Fail("bad PE signature");
    IMAGE_DATA_DIRECTORY rsrc{};
    std::memcpy(&rsrc, &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE], sizeof rsrc);
    if (rsrc.VirtualAddress == 0 || rsrc.Size == 0) return Fail("no resource section");
    uint64_t rsrcBase = base + rsrc.VirtualAddress;

    struct RsrcDir { uint32_t characteristics; uint32_t timeDateStamp; uint16_t majorVersion, minorVersion; uint16_t numberOfNamedEntries, numberOfIdEntries; };
    struct RsrcEntry { uint32_t name; uint32_t data; };
    struct RsrcLeaf { uint32_t offsetToData; uint32_t size; uint32_t timeDateStamp; uint32_t codePage; uint32_t characteristics; };

    auto ReadDir = [&](uint64_t rva, std::vector<RsrcEntry>& out) -> bool {
        std::vector<BYTE> db;
        if (!ReadAt(rva, sizeof(RsrcDir), db)) return false;
        RsrcDir d{};
        std::memcpy(&d, db.data(), sizeof d);
        uint32_t count = d.numberOfNamedEntries + d.numberOfIdEntries;
        if (count == 0 || count > 64) return false;
        std::vector<BYTE> eb;
        if (!ReadAt(rva + sizeof(RsrcDir), (size_t)count * sizeof(RsrcEntry), eb)) return false;
        out.resize(count);
        std::memcpy(out.data(), eb.data(), eb.size());
        return true;
    };
    auto IsDir  = [](const RsrcEntry& x) { return (x.data & 0x80000000u) != 0; };
    auto DirOff = [](const RsrcEntry& x) -> uint64_t { return (uint64_t)(x.data & 0x7FFFFFFFu); };

    std::vector<RsrcEntry> e;
    if (!ReadDir(rsrcBase, e)) return Fail("cannot read resource root directory");
    const RsrcEntry* ver = nullptr;
    for (const auto& x : e)
        if (!(x.name & 0x80000000u) && (x.name & 0xFFFF) == 16 && IsDir(x)) { ver = &x; break; }
    if (!ver) return Fail("no RT_VERSION resource");
    if (!ReadDir(rsrcBase + DirOff(*ver), e)) return Fail("cannot read language directory");
    if (e.empty() || !IsDir(e[0])) return Fail("unexpected language directory");
    uint64_t idDir = rsrcBase + DirOff(e[0]);
    if (!ReadDir(idDir, e)) return Fail("cannot read data directory");
    if (e.empty() || IsDir(e[0])) return Fail("expected a leaf data entry");
    std::vector<BYTE> lb;
    if (!ReadAt(idDir + sizeof(RsrcDir), sizeof(RsrcLeaf), lb)) return Fail("cannot read resource data entry");
    RsrcLeaf leaf{};
    std::memcpy(&leaf, lb.data(), sizeof leaf);
    uint64_t offToData = leaf.offsetToData;
    uint64_t dataSize = leaf.size;
    if (dataSize < 8 || dataSize > (16u * 1024 * 1024)) return Fail("implausible VS_VERSION_INFO size");
    std::vector<BYTE> data;
    if (!ReadAt(base + offToData, dataSize, data)) return Fail("cannot read VS_VERSION_INFO data");
    ParseVerNode(data.data(), 0, data.size(), v, 0);
    v.ok = v.fver_ms || v.pver_ms || !v.strings.empty();
    if (v.ok) v.via = L"memory";
    CloseHandle(h);
    return v;
}

static VersionInfo ResolveModuleVersion(const ModuleInfo& m, DWORD pid) {
    VersionInfo v = ReadFileVersionInfo(m.path);
    if (v.ok) return v;
    VersionInfo mem = ReadVersionFromMemory(pid, m.base);
    if (mem.ok) {
        mem.error = L"file read failed (" + v.error + L"); recovered from the in-memory version resource";
        return mem;
    }
    v.error = L"file read failed (" + v.error + L"); in-memory walk failed (" + mem.error + L")";
    return v;
}

static std::wstring PickVersionString(const VersionInfo& v, const std::wstring& p1, const std::wstring& p2) {
    auto get = [&](const std::wstring& k) -> std::wstring {
        auto it = v.strings.find(k);
        return it == v.strings.end() ? std::wstring() : it->second;
    };
    std::wstring a = get(p1);
    if (!a.empty()) return a;
    std::wstring b = get(p2);
    if (!b.empty()) return b;
    if (v.ok) {
        std::wstring pv = FormatVersion(v.pver_ms, v.pver_ls);
        if (pv != L"0.0.0.0") return pv;
        std::wstring fv = FormatVersion(v.fver_ms, v.fver_ls);
        if (fv != L"0.0.0.0") return fv;
    }
    return std::wstring();
}

// ---------------------------------------------------------------------------
// Appx package identity
// ---------------------------------------------------------------------------
struct PkgInfo {
    std::wstring name;
    std::wstring family;
    std::wstring full;
    std::wstring version;
    std::wstring path;
    bool ok = false;
    LONG err = 0;
};

static std::wstring IdentityNameFromFullName(const std::wstring& full) {
    size_t u = full.find('_');
    return (u == std::wstring::npos) ? full : full.substr(0, u);
}

static PkgInfo GetPackageForProcess(DWORD pid) {
    PkgInfo p;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { p.err = (LONG)GetLastError(); return p; }
    UINT32 n = 0;
    LONG rc = GetPackageFamilyName(h, &n, nullptr);
    if (rc == ERROR_INSUFFICIENT_BUFFER && n > 0) {
        std::vector<wchar_t> b(n);
        if (GetPackageFamilyName(h, &n, b.data()) == ERROR_SUCCESS) p.family = b.data();
    } else {
        p.err = rc; CloseHandle(h); return p;
    }
    n = 0;
    if (GetPackageFullName(h, &n, nullptr) == ERROR_INSUFFICIENT_BUFFER && n > 0) {
        std::vector<wchar_t> b(n);
        if (GetPackageFullName(h, &n, b.data()) == ERROR_SUCCESS) p.full = b.data();
    }
    CloseHandle(h);
    if (!p.full.empty()) {
        std::vector<std::wstring> parts;
        {
            size_t start = 0;
            for (size_t i = 0; i <= p.full.size(); ++i) {
                if (i == p.full.size() || p.full[i] == L'_') {
                    parts.push_back(p.full.substr(start, i - start));
                    start = i + 1;
                }
            }
        }
        auto Safe = [](const std::wstring& s) {
            return !s.empty() && s.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
        };
        if (parts.size() >= 1 && Safe(parts[0])) p.name = parts[0];
        if (parts.size() >= 2 && Safe(parts[1])) p.version = parts[1];
        UINT32 pl = 0;
        if (GetPackagePathByFullName(p.full.c_str(), &pl, nullptr) == ERROR_INSUFFICIENT_BUFFER && pl > 0) {
            std::vector<wchar_t> b(pl);
            if (GetPackagePathByFullName(p.full.c_str(), &pl, b.data()) == ERROR_SUCCESS)
                p.path = b.data();
        }
    }
    if (p.name.empty() && !p.full.empty()) p.name = IdentityNameFromFullName(p.full);
    p.ok = !p.family.empty();
    return p;
}

struct AppxPackage {
    std::wstring name;
    std::wstring family;
    std::wstring version;
    std::wstring full_name;
    std::wstring install_location;
};

static bool ListArkPackagesInstalled(std::vector<AppxPackage>& out, std::string& detail) {
    static const char kScript[] =
        "$all = @()\n"
        "try { $all = @(Get-AppxPackage -AllUsers 2>$null) } catch { }\n"
        "if ($all.Count -eq 0) { $all = @(Get-AppxPackage 2>$null) }\n"
        "$pkgs = @($all | Where-Object { $_.Name -like '*ARK*' })\n"
        "if ($pkgs.Count -eq 0) {\n"
        "  $hits = @()\n"
        "  foreach ($p in $all) {\n"
        "    try {\n"
        "      $dn = (Get-AppxPackageManifest $p).Package.Properties.DisplayName\n"
        "      if ($dn -like '*ARK*') { $hits += $p }\n"
        "    } catch { }\n"
        "  }\n"
        "  $pkgs = $hits\n"
        "}\n"
        "if ($pkgs) {\n"
        "  foreach ($p in $pkgs) {\n"
        "    [System.Console]::Out.WriteLine((@($p.Name, $p.PackageFamilyName, [string]$p.Version, $p.PackageFullName, $p.InstallLocation) -join \"`t\"))\n"
        "  }\n"
        "}\n";
    wchar_t tmp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) { detail = "GetTempPathW failed"; return false; }
    std::wstring ps1Path = std::wstring(tmp) + L"ark_env_appx_query.ps1";
    {
        HANDLE hf = CreateFileW(ps1Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) { detail = "cannot write temp ps1"; return false; }
        DWORD w = 0;
        WriteFile(hf, kScript, (DWORD)(sizeof(kScript) - 1), &w, nullptr);
        CloseHandle(hf);
    }
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE hR = nullptr, hW = nullptr;
    if (!CreatePipe(&hR, &hW, &sa, 0)) { detail = "CreatePipe failed"; DeleteFileW(ps1Path.c_str()); return false; }
    SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + ps1Path + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hW; si.hStdError = hW;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        detail = "CreateProcess failed"; CloseHandle(hR); CloseHandle(hW); DeleteFileW(ps1Path.c_str()); return false;
    }
    CloseHandle(hW);
    std::string output; char buf[4096];
    for (;;) { DWORD rd = 0; if (!ReadFile(hR, buf, sizeof buf, &rd, nullptr) || rd == 0) break; output.append(buf, rd); }
    CloseHandle(hR); WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 0; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); DeleteFileW(ps1Path.c_str());
    size_t pos = 0;
    while (pos < output.size()) {
        size_t nl = output.find('\n', pos);
        std::string line = (nl == std::string::npos) ? output.substr(pos) : output.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? output.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> f; size_t s = 0;
        while (s < line.size()) { size_t t = line.find('\t', s); f.push_back(line.substr(s, t - s)); s = (t == std::string::npos) ? line.size() : t + 1; }
        if (f.size() < 2) continue;
        AppxPackage p;
        p.name = ToWide(f[0]); p.family = ToWide(f.size() > 1 ? f[1] : std::string());
        p.version = ToWide(f.size() > 2 ? f[2] : std::string()); p.full_name = ToWide(f.size() > 3 ? f[3] : std::string());
        p.install_location = ToWide(f.size() > 4 ? f[4] : std::string());
        out.push_back(std::move(p));
    }
    detail = "parsed " + std::to_string(out.size()) + " package(s)";
    return true;
}

// ---------------------------------------------------------------------------
// Window & Monitors
// ---------------------------------------------------------------------------
struct WindowInfo {
    bool found = false; UINT_PTR hwnd = 0; std::wstring title; std::wstring class_name;
    LONG left = 0, top = 0, width = 0, height = 0; LONG client_w = 0, client_h = 0; UINT dpi = 0;
};

static void FillWindowInfo(HWND h, WindowInfo& wi) {
    wi.found = true; wi.hwnd = (UINT_PTR)h;
    wchar_t t[512] = {}; GetWindowTextW(h, t, 511); wi.title = t;
    wchar_t c[256] = {}; GetClassNameW(h, c, 255); wi.class_name = c;
    RECT r{}; if (GetWindowRect(h, &r)) { wi.left = r.left; wi.top = r.top; wi.width = r.right - r.left; wi.height = r.bottom - r.top; }
    RECT cr{}; if (GetClientRect(h, &cr)) { wi.client_w = cr.right; wi.client_h = cr.bottom; }
    wi.dpi = GetWindowDpi(h);
}

struct WinPick { DWORD pid = 0; HWND best = nullptr; LONG bestArea = -1; };

static BOOL CALLBACK WinPickProc(HWND h, LPARAM lp) {
    auto* p = reinterpret_cast<WinPick*>(lp);
    DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
    if (wpid == p->pid) {
        RECT r{}; if (GetWindowRect(h, &r)) {
            LONG area = (LONG)((LONG64)(r.right - r.left) * (LONG64)(r.bottom - r.top));
            if (area > p->bestArea) { p->bestArea = area; p->best = h; }
        }
        return TRUE;
    }
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 63);
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) EnumChildWindows(h, WinPickProc, lp);
    return TRUE;
}

static WindowInfo FindGameWindow(DWORD pid, UINT_PTR hwndForce) {
    WindowInfo wi;
    if (hwndForce != 0) { HWND h = (HWND)hwndForce; if (IsWindow(h)) FillWindowInfo(h, wi); return wi; }
    WinPick pick; pick.pid = pid; EnumWindows(WinPickProc, (LPARAM)&pick);
    if (!pick.best) return wi;
    FillWindowInfo(pick.best, wi); return wi;
}

struct MonitorInfo { LONG left = 0, top = 0, width = 0, height = 0; bool primary = false; };
struct MonCtx { std::vector<MonitorInfo> mons; };

static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* c = reinterpret_cast<MonCtx*>(lp);
    MONITORINFO mi{}; mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(hMon, &mi))
        c->mons.push_back({mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

static std::vector<MonitorInfo> EnumMonitors() {
    MonCtx ctx; EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&ctx); return std::move(ctx.mons);
}

struct GameMonitor {
    bool found = false; std::wstring device; LONG left = 0, top = 0, width = 0, height = 0; UINT pels_w = 0, pels_h = 0, refresh = 0;
};

static GameMonitor GetGameMonitor(HWND hwnd) {
    GameMonitor g; HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
    if (!hm || !GetMonitorInfoW(hm, &mi)) return g;
    g.device = mi.szDevice; g.left = mi.rcMonitor.left; g.top = mi.rcMonitor.top;
    g.width = mi.rcMonitor.right - mi.rcMonitor.left; g.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DEVMODEW dm{}; dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        g.pels_w = dm.dmPelsWidth; g.pels_h = dm.dmPelsHeight; g.refresh = dm.dmDisplayFrequency;
    }
    g.found = true; return g;
}

static bool IsAppContainer(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ac = false; HANDLE tok = nullptr;
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        DWORD isAC = 0, cb = 0;
        if (GetTokenInformation(tok, TokenIsAppContainer, &isAC, sizeof isAC, &cb)) ac = (isAC != 0);
        CloseHandle(tok);
    }
    CloseHandle(h); return ac;
}

// ---------------------------------------------------------------------------
// Graded access self-test
// ---------------------------------------------------------------------------
struct AccessTier { std::string label; DWORD rights = 0; bool ok = false; DWORD err = 0; };
struct AccessSelfTest {
    bool ran = false; uint64_t addr = 0; std::wstring addr_note; std::vector<AccessTier> tiers;
    std::string read_rights_label; bool read_ok = false; DWORD read_err = 0;
    uint64_t requested_bytes = 65536; uint64_t attempted_bytes = 0; uint64_t bytes_read = 0;
    std::string magic; std::wstring verdict;
};

static AccessSelfTest RunAccessSelfTest(DWORD pid, uint64_t baseAddr, const std::wstring& addrNote) {
    AccessSelfTest st; st.addr = baseAddr; st.addr_note = addrNote;
    st.tiers = {
        { "PROCESS_ALL_ACCESS", PROCESS_ALL_ACCESS, false, 0 },
        { "PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION", PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, false, 0 },
        { "PROCESS_VM_READ|PROCESS_QUERY_INFORMATION", PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, 0 },
        { "PROCESS_QUERY_LIMITED_INFORMATION", PROCESS_QUERY_LIMITED_INFORMATION, false, 0 },
    };
    HANDLE handles[4] = { nullptr, nullptr, nullptr, nullptr };
    for (size_t i = 0; i < st.tiers.size(); ++i) {
        handles[i] = OpenProcess(st.tiers[i].rights, FALSE, pid);
        if (handles[i]) st.tiers[i].ok = true; else st.tiers[i].err = GetLastError();
    }
    size_t readIdx = (size_t)-1;
    for (size_t i = 0; i + 1 < st.tiers.size(); ++i) if (st.tiers[i].ok) { readIdx = i; break; }
    if (readIdx != (size_t)-1) {
        HANDLE h = handles[readIdx]; st.read_rights_label = st.tiers[readIdx].label;
        const SIZE_T wantMax = 65536; SIZE_T contiguous = 0;
        for (;;) {
            if (contiguous >= wantMax) break;
            MEMORY_BASIC_INFORMATION mbi{}; void* probe = reinterpret_cast<void*>(baseAddr + contiguous);
            if (VirtualQueryEx(h, probe, &mbi, sizeof mbi) != sizeof mbi) break;
            if (mbi.State != MEM_COMMIT) break;
            uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            uintptr_t addr = (uintptr_t)probe;
            if (regionEnd <= addr) break;
            contiguous += (SIZE_T)(regionEnd - addr);
        }
        SIZE_T want = (contiguous > 0 && contiguous < wantMax) ? contiguous : wantMax;
        st.attempted_bytes = (uint64_t)want;
        std::vector<BYTE> buf((size_t)want); SIZE_T got = 0;
        if (ReadProcessMemory(h, reinterpret_cast<LPCVOID>(baseAddr), buf.data(), (DWORD)want, &got)) {
            st.read_ok = true; st.bytes_read = (uint64_t)got;
            if (got >= 2) st.magic = std::string(1, (char)buf[0]) + std::string(1, (char)buf[1]);
        } else { st.read_err = GetLastError(); }
    }
    for (HANDLE h : handles) if (h) CloseHandle(h);
    if (st.read_ok && st.bytes_read >= 65536)
        st.verdict = L"PASS - elevated access confirmed (OpenProcess + 64 KiB read, " + std::to_wstring((unsigned long)st.bytes_read) + L" bytes)";
    else if (st.read_ok)
        st.verdict = L"PARTIAL - read returned " + std::to_wstring((unsigned long)st.bytes_read) + L" of " + std::to_wstring((unsigned long)st.attempted_bytes) + L" requested bytes";
    else if (readIdx != (size_t)-1)
        st.verdict = L"FAIL - ReadProcessMemory denied (error " + std::to_wstring((unsigned long)st.read_err) + L")";
    else
        st.verdict = L"FAIL - no OpenProcess rights tier succeeded (last error " + std::to_wstring((unsigned long)st.tiers.back().err) + L")";
    st.ran = true; return st;
}

// ---------------------------------------------------------------------------
// Report + JSON
// ---------------------------------------------------------------------------
struct ModFail { DWORD pid = 0; std::string name; DWORD winErr = 0; };
struct Report {
    std::string os_string, timestamp_utc, status, message;
    bool self_elevated = false, se_debug = false, game_found = false, target_appcontainer = false, monolithic = false, d3d_found = false;
    unsigned debug_err = 0; DWORD proc_pid = 0; int candidate_count = 0, primary_w = 0, primary_h = 0;
    std::wstring proc_name, proc_path, engine_name, game_name, disambig_method, disambig_note, d3d_flavor;
    ModuleInfo engine_mod, game_mod, d3d_mod; std::vector<ModuleInfo> all_mods;
    VersionInfo engine_ver, game_ver, d3d_ver;
    std::string engine_version_string, ark_version_string, d3d_version_string, appx_source = "none", appx_detail;
    std::vector<AppxPackage> appx; WindowInfo window; std::vector<MonitorInfo> monitors; GameMonitor game_monitor;
    AccessSelfTest selftest; std::vector<std::string> notes; size_t mod_fail_total = 0; std::vector<ModFail> mod_failures;
};

static std::string VersionToJsonInline(const VersionInfo& v, const std::string& chosen) {
    static const std::pair<const wchar_t*, const char*> kFields[] = {
        {L"CompanyName", "company_name"}, {L"FileDescription", "file_description"}, {L"FileVersion", "file_version"},
        {L"InternalName", "internal_name"}, {L"LegalCopyright", "legal_copyright"}, {L"LegalTrademarks", "legal_trademarks"},
        {L"OriginalFilename", "original_filename"}, {L"PrivateBuild", "private_build"}, {L"ProductName", "product_name"},
        {L"ProductVersion", "product_version"},
    };
    std::ostringstream o;
    o << "{ \"ok\": " << (v.ok ? "true" : "false") << ", \"version_source\": \"" << JEsc(v.via) << "\"";
    if (v.ok)
        o << ", \"file_version_fixed\": \"" << JEsc(ToUtf8(FormatVersion(v.fver_ms, v.fver_ls))) << "\""
          << ", \"product_version_fixed\": \"" << JEsc(ToUtf8(FormatVersion(v.pver_ms, v.pver_ls))) << "\"";
    for (const auto& kv : v.strings) {
        for (const auto& f : kFields) {
            if (kv.first == f.first) { o << ", \"" << f.second << "\": \"" << JEsc(kv.second) << "\""; break; }
        }
    }
    o << ", \"version_string\": \"" << JEsc(chosen) << "\"";
    if (!v.error.empty()) o << ", \"error\": \"" << JEsc(v.error) << "\"";
    o << " }";
    return o.str();
}

static std::string BuildJson(const Report& r) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema_version\": 1,\n";
    o << "  \"tool\": { \"name\": \"ark-env\", \"version\": \"1.1.0\", "
      << "\"part\": \"1: Environment Discovery & Process Identification\", "
      << "\"platform\": \"" << JEsc(r.os_string) << "\", "
      << "\"timestamp_utc\": \"" << r.timestamp_utc << "\", "
      << "\"self_is_elevated\": " << (r.self_elevated ? "true" : "false")
      << ", \"se_debug_privilege\": " << (r.se_debug ? "true" : "false") << " },\n";
    o << "  \"summary\": { \"status\": \"" << JEsc(r.status) << "\", \"message\": \"" << JEsc(r.message) << "\" },\n";
    o << "  \"game_process\": { \"found\": " << (r.game_found ? "true" : "false");
    if (r.game_found) {
        o << ", \"name\": \"" << JEsc(r.proc_name) << "\""
          << ", \"pid\": " << (unsigned long)r.proc_pid
          << ", \"image_path\": \"" << JEsc(r.proc_path) << "\""
          << ", \"is_app_container\": " << (r.target_appcontainer ? "true" : "false")
          << ", \"disambiguation\": { \"method\": \"" << JEsc(r.disambig_method) << "\""
          << ", \"engine_module\": \"" << JEsc(r.engine_name) << "\""
          << ", \"game_module\": \"" << JEsc(r.game_name)
          << "\", \"monolithic\": " << (r.monolithic ? "true" : "false")
          << ", \"candidate_processes\": " << r.candidate_count
          << ", \"note\": \"" << JEsc(r.disambig_note) << "\" }";
    }
    o << " },\n";
    o << "  \"modules\": { ";
    if (r.game_found) {
        o << "\"engine\": { \"name\": \"" << JEsc(r.engine_mod.name)
          << "\", \"path\": \"" << JEsc(r.engine_mod.path)
          << "\", \"base\": \"" << JHex(r.engine_mod.base) << "\", \"base_dec\": " << r.engine_mod.base
          << ", \"size_bytes\": " << r.engine_mod.size
          << ", \"version\": " << VersionToJsonInline(r.engine_ver, r.engine_version_string) << " }, ";
        o << "\"game\": { \"name\": \"" << JEsc(r.game_mod.name)
          << "\", \"path\": \"" << JEsc(r.game_mod.path)
          << "\", \"base\": \"" << JHex(r.game_mod.base) << "\", \"base_dec\": " << r.game_mod.base
          << ", \"size_bytes\": " << r.game_mod.size
          << ", \"version\": " << VersionToJsonInline(r.game_ver, r.ark_version_string) << " }, ";
        o << "\"d3d\": { \"found\": " << (r.d3d_found ? "true" : "false")
          << ", \"flavor\": \"" << JEsc(r.d3d_flavor) << "\""
          << ", \"name\": \"" << JEsc(r.d3d_mod.name)
          << "\", \"base\": \"" << JHex(r.d3d_mod.base) << "\", \"size_bytes\": " << r.d3d_mod.size
          << ", \"version_string\": \"" << JEsc(r.d3d_version_string) << "\""
          << ", \"version_error\": \"" << JEsc(r.d3d_ver.error) << "\" }, ";
        o << "\"all\": [";
        for (size_t i = 0; i < r.all_mods.size(); ++i) {
            o << (i ? ", " : "") << "{ \"name\": \"" << JEsc(r.all_mods[i].name)
              << "\", \"base\": \"" << JHex(r.all_mods[i].base)
              << "\", \"size_bytes\": " << r.all_mods[i].size << " }";
        }
        o << " ]";
    } else {
        o << "\"engine\": null, \"game\": null, \"d3d\": null, \"all\": []";
    }
    o << " },\n";
    o << "  \"appx\": { \"source\": \"" << JEsc(r.appx_source) << "\""
      << ", \"detail\": \"" << JEsc(r.appx_detail) << "\""
      << ", \"packages\": [";
    for (size_t i = 0; i < r.appx.size(); ++i) {
        const auto& p = r.appx[i];
        o << (i ? ", " : "") << "{ \"name\": \"" << JEsc(p.name)
          << "\", \"package_family_name\": \"" << JEsc(p.family)
          << "\", \"version\": \"" << JEsc(p.version)
          << "\", \"package_full_name\": \"" << JEsc(p.full_name)
          << "\", \"install_location\": \"" << JEsc(p.install_location) << "\" }";
    }
    o << " ] },\n";
    o << "  \"window\": { \"found\": " << (r.window.found ? "true" : "false");
    if (r.window.found) {
        o << ", \"hwnd\": \"" << JHexPtr(r.window.hwnd) << "\""
          << ", \"class_name\": \"" << JEsc(r.window.class_name) << "\""
          << ", \"title\": \"" << JEsc(r.window.title) << "\""
          << ", \"dpi\": " << (unsigned long)r.window.dpi
          << ", \"rect\": { \"left\": " << (long)r.window.left
          << ", \"top\": " << (long)r.window.top
          << ", \"width\": " << (long)r.window.width
          << ", \"height\": " << (long)r.window.height << " }"
          << ", \"client_size\": { \"width\": " << (long)r.window.client_w
          << ", \"height\": " << (long)r.window.client_h << " }";
    }
    o << " },\n";
    
    // DISPLAY OBJECT
    o << "  \"display\": { \"primary_resolution\": { \"width\": " << (long)r.primary_w
      << ", \"height\": " << (long)r.primary_h << " }, "
      << "\"game_monitor\": { \"found\": " << (r.game_monitor.found ? "true" : "false")
      << ", \"device\": \"" << JEsc(r.game_monitor.device) << "\""
      << ", \"left\": " << (long)r.game_monitor.left
      << ", \"top\": " << (long)r.game_monitor.top
      << ", \"width\": " << (long)r.game_monitor.width
      << ", \"height\": " << (long)r.game_monitor.height
      << ", \"mode\": { \"width\": " << (unsigned long)r.game_monitor.pels_w
      << ", \"height\": " << (unsigned long)r.game_monitor.pels_h
      << ", \"refresh_hz\": " << (unsigned long)r.game_monitor.refresh << " }"
      << ", \"monitors\": [";
    for (size_t i = 0; i < r.monitors.size(); ++i) {
        const auto& m = r.monitors[i];
        o << (i ? ", " : "") << "{ \"left\": " << (long)m.left << ", \"top\": " << (long)m.top
          << ", \"width\": " << (long)m.width << ", \"height\": " << (long)m.height
          << ", \"primary\": " << (m.primary ? "true" : "false") << " }";
    }
    o << " ] } },\n"; // <--- THIS IS THE FIX: Added the missing } for the "display" object
    
    o << "  \"access_self_test\": { \"ran\": " << (r.selftest.ran ? "true" : "false")
      << ", \"tiers\": [";
    for (size_t i = 0; i < r.selftest.tiers.size(); ++i) {
        const auto& t = r.selftest.tiers[i];
        o << (i ? ", " : "") << "{ \"label\": \"" << JEsc(t.label)
          << "\", \"rights\": \"" << JHex((uint64_t)t.rights) << "\""
          << ", \"success\": " << (t.ok ? "true" : "false")
          << ", \"error_code\": " << (unsigned long)t.err << " }";
    }
    o << " ], \"read_memory\": { \"address\": \"" << JHex(r.selftest.addr) << "\""
      << ", \"address_note\": \"" << JEsc(r.selftest.addr_note) << "\""
      << ", \"rights_used\": \"" << JEsc(r.selftest.read_rights_label) << "\""
      << ", \"requested_bytes\": " << (unsigned long)r.selftest.requested_bytes
      << ", \"attempted_bytes\": " << r.selftest.attempted_bytes
      << ", \"bytes_read\": " << r.selftest.bytes_read
      << ", \"success\": " << (r.selftest.read_ok ? "true" : "false")
      << ", \"magic\": \"" << JEsc(r.selftest.magic) << "\""
      << ", \"error_code\": " << (unsigned long)r.selftest.read_err << " }"
      << ", \"verdict\": \"" << JEsc(r.selftest.verdict) << "\" },\n";
    o << "  \"diagnostics\": { \"notes\": [";
    for (size_t i = 0; i < r.notes.size(); ++i)
        o << (i ? ", " : "") << "\"" << JEsc(r.notes[i]) << "\"";
    o << " ], \"module_enum_failures\": { \"total\": " << r.mod_fail_total
      << ", \"listed\": [";
    for (size_t i = 0; i < r.mod_failures.size(); ++i) {
        const auto& f = r.mod_failures[i];
        o << (i ? ", " : "") << "{ \"pid\": " << (unsigned long)f.pid
          << ", \"name\": \"" << JEsc(f.name)
          << "\", \"win32_error\": " << (unsigned long)f.winErr << " }";
    }
    o << " ] } }\n}\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Candidate building
// ---------------------------------------------------------------------------
struct Candidate {
    DWORD pid = 0; std::wstring exe, image; ModuleInfo engine, game; size_t modCount = 0; bool hasD3D = false;
    std::vector<ModuleInfo> mods; std::wstring method; PkgInfo pkg;
};

static bool TryCandidate(DWORD pid, const std::wstring& exeName, const std::wstring& engOvr, const std::wstring& gamOvr, Report& r, Candidate& c, std::string& why) {
    c = Candidate{};
    std::vector<ModuleInfo> mods; DWORD winErr = 0;
    if (!ListProcessModules(pid, mods, winErr)) {
        ++r.mod_fail_total;
        if (r.mod_failures.size() < 64) r.mod_failures.push_back({pid, ToUtf8(exeName), winErr});
        why = "module enumeration failed (error " + std::to_string(winErr) + ")"; return false;
    }
    auto isEng = [&](const std::wstring& n) { std::wstring s = ToLower(n); return engOvr.empty() ? IsEngineDll(n) : (s == engOvr); };
    auto isGam = [&](const std::wstring& n) { std::wstring s = ToLower(n); return gamOvr.empty() ? IsGameDll(n) : (s == gamOvr); };
    const ModuleInfo* eng = nullptr; const ModuleInfo* gam = nullptr;
    for (const auto& m : mods) { if (!eng && isEng(m.name)) eng = &m; if (!gam && isGam(m.name)) gam = &m; }
    bool hasD3D = false;
    for (const auto& m : mods) if (IsD3D11(m.name) || IsD3D12(m.name)) hasD3D = true;
    if (eng && gam) {
        c.pid = pid; c.exe = exeName; c.image = GetProcessImage(pid); c.engine = *eng; c.game = *gam;
        c.modCount = mods.size(); c.hasD3D = hasD3D; c.mods = std::move(mods); c.method = L"module_set"; return true;
    }
    PkgInfo pk = GetPackageForProcess(pid);
    if (pk.ok && ToLower(pk.family).find(L"studiowildcard") != std::wstring::npos) {
        c.pid = pid; c.exe = exeName; c.image = GetProcessImage(pid);
        if (!mods.empty()) {
            const ModuleInfo* img = nullptr;
            for (const auto& m : mods) if (ToLower(m.name) == ToLower(exeName)) { img = &m; break; }
            if (!img) img = &mods[0];
            c.engine = *img; c.game = *img;
        }
        c.modCount = mods.size(); c.hasD3D = hasD3D; c.mods = std::move(mods); c.method = L"monolithic_package_identity"; c.pkg = pk; return true;
    }
    why = "no engine/game marker modules and no StudioWildcard package identity"; return false;
}

} // namespace ark

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    using namespace ark;
    SetDpiAwareness();
    std::string outPath = "ark_env.json"; DWORD forcePid = 0; UINT_PTR hwndForce = 0;
    std::wstring engineOverride, gameOverride; bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--pid" && i + 1 < argc) forcePid = (DWORD)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--hwnd" && i + 1 < argc) hwndForce = (UINT_PTR)std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--engine" && i + 1 < argc) engineOverride = ToLower(ToWide(argv[++i]));
        else if (a == "--game" && i + 1 < argc) gameOverride = ToLower(ToWide(argv[++i]));
        else if (a == "--quiet") quiet = true;
        else if (a == "--help" || a == "-h" || a == "/?") {
            std::printf("ark-env - PART 1 environment discovery & process identification\n"
                        "usage: ark_env.exe [--out FILE] [--pid N] [--engine NAME] [--game NAME]\n"
                        "                    [--hwnd 0xNNN] [--quiet]\n");
            return 0;
        }
    }
    if (!quiet) SetConsoleOutputCP(CP_UTF8);
    DWORD dbgErr = 0; bool seDebug = EnableDebugPrivilege(dbgErr);
    Report r; r.timestamp_utc = UtcNowIso8601(); r.self_elevated = IsElevated(); r.se_debug = seDebug;
    r.debug_err = (unsigned)dbgErr; r.os_string = OsString(GetOsVersion());

    std::vector<ProcEntry> procs;
    if (!EnumAllProcesses(procs)) { r.status = "FATAL"; r.message = "CreateToolhelp32Snapshot failed"; }

    std::vector<Candidate> cands;
    for (const auto& p : procs) {
        std::string why; Candidate c;
        if (TryCandidate(p.pid, p.name, engineOverride, gameOverride, r, c, why)) cands.push_back(std::move(c));
    }
    r.candidate_count = (int)cands.size();

    if (forcePid != 0) {
        bool found = false;
        for (const auto& c : cands) if (c.pid == forcePid) found = true;
        if (!found) {
            std::string why; Candidate c;
            if (TryCandidate(forcePid, L"(forced --pid)", engineOverride, gameOverride, r, c, why)) cands.push_back(std::move(c));
            else { r.status = "FATAL"; r.message = "forced --pid " + std::to_string(forcePid) + " is not an ARK candidate: " + why; }
        }
    }

    if (!cands.empty()) {
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
            bool am = (a.method == L"module_set"); bool bm = (b.method == L"module_set");
            if (am != bm) return am; if (a.hasD3D != b.hasD3D) return a.hasD3D;
            if (a.game.size != b.game.size) return a.game.size > b.game.size; return a.modCount > b.modCount;
        });
        const auto& best = cands[0];
        r.game_found = true; r.proc_pid = best.pid; r.proc_name = best.exe;
        r.proc_path = best.image.empty() ? GetProcessImage(best.pid) : best.image;
        r.engine_mod = best.engine; r.game_mod = best.game; r.all_mods = best.mods;
        r.engine_name = best.engine.name; r.game_name = best.game.name; r.monolithic = (r.engine_name == r.game_name);
        r.disambig_method = best.method;
        r.disambig_note = r.monolithic ? L"engine and game module are the same image; the two-DLL rule from the card does not apply to this monolithic Store build" : L"process loads both the UE engine marker DLL and the ShooterGame marker DLL";
        for (const auto& m : r.all_mods) {
            if (IsD3D11(m.name)) { r.d3d_found = true; r.d3d_mod = m; r.d3d_flavor = L"d3d11"; break; }
            if (IsD3D12(m.name)) { r.d3d_found = true; r.d3d_mod = m; r.d3d_flavor = L"d3d12"; }
        }
        r.target_appcontainer = IsAppContainer(best.pid);
    }

    if (r.game_found) {
        r.engine_ver = ResolveModuleVersion(r.engine_mod, r.proc_pid);
        r.game_ver = ResolveModuleVersion(r.game_mod, r.proc_pid);
        r.engine_version_string = ToUtf8(PickVersionString(r.engine_ver, L"ProductVersion", L"FileDescription"));
        r.ark_version_string = ToUtf8(PickVersionString(r.game_ver, L"ProductVersion", L"FileVersion"));
        if (r.d3d_found) {
            r.d3d_ver = ResolveModuleVersion(r.d3d_mod, r.proc_pid);
            r.d3d_version_string = ToUtf8(PickVersionString(r.d3d_ver, L"ProductVersion", L"FileVersion"));
        }
    }

    if (r.game_found) {
        PkgInfo pk = GetPackageForProcess(r.proc_pid);
        if (pk.ok) {
            r.appx_source = "appmodel API (GetPackageFamilyName / GetPackageFullName) tied to game PID " + std::to_string(r.proc_pid);
            r.appx_detail = "package identity read from the game process handle; version parsed from the package full name";
            AppxPackage a; a.name = pk.name; a.family = pk.family; a.version = pk.version; a.full_name = pk.full; a.install_location = pk.path;
            r.appx.push_back(std::move(a));
            if (r.ark_version_string.empty() && !pk.version.empty()) {
                r.ark_version_string = ToUtf8(pk.version);
                r.notes.push_back("NOTE: ARK version string taken from the package identity (PackageIdFromFullName) because the module's version resource was unreadable");
            }
        } else {
            r.appx_source = "none (unpackaged build)";
            r.appx_detail = "GetPackageFamilyName returned " + std::to_string((unsigned long)pk.err);
        }
    } else {
        std::string psDetail;
        if (ListArkPackagesInstalled(r.appx, psDetail)) { r.appx_source = "powershell"; r.appx_detail = psDetail; }
        else { r.appx_detail = "system-wide ARK package listing failed: " + psDetail; }
    }

    if (r.game_found && r.monolithic && r.engine_version_string.empty() && !r.ark_version_string.empty())
        r.engine_version_string = r.ark_version_string;

    if (r.game_found) {
        r.window = FindGameWindow(r.proc_pid, hwndForce);
        if (hwndForce != 0 && r.window.found) r.notes.push_back("NOTE: window handle was supplied via --hwnd");
        if (r.window.found) r.game_monitor = GetGameMonitor((HWND)r.window.hwnd);
    }
    r.monitors = EnumMonitors(); r.primary_w = GetSystemMetrics(SM_CXSCREEN); r.primary_h = GetSystemMetrics(SM_CYSCREEN);

    if (r.game_found) {
        uint64_t base = 0; std::wstring note;
        if (r.engine_mod.base) { base = r.engine_mod.base; note = L"module base (" + r.engine_name + L")"; }
        else if (r.game_mod.base) { base = r.game_mod.base; note = L"module base (" + r.game_name + L")"; }
        else { base = 0x10000; note = L"fallback address 0x10000"; }
        r.selftest = RunAccessSelfTest(r.proc_pid, base, note);
    }

    if (!r.status.empty()) {}
    else if (!r.game_found) { r.status = "NOT_FOUND"; r.message = "No ARK process found."; }
    else if (r.selftest.read_ok && r.selftest.bytes_read >= 65536) { r.status = "OK"; r.message = "ARK process identified; elevated access confirmed."; }
    else { r.status = "OK_SELFTEST_FAIL"; r.message = "ARK process identified but the access self-test failed."; }

    if (!r.self_elevated) r.notes.push_back("NOTE: this tool is NOT running elevated.");
    if (!r.se_debug && r.debug_err == 1300) r.notes.push_back("NOTE: SeDebugPrivilege is present but disabled.");
    if (r.game_found && r.monolithic) r.notes.push_back("NOTE: monolithic Store build - engine_version and ark_version are the same string by design.");
    if (r.game_found && !r.window.found) r.notes.push_back("NOTE: no window matched the game PID.");
    if (r.game_found && !r.d3d_found) r.notes.push_back("NOTE: neither d3d11.dll nor d3d12.dll found.");
    if (r.game_found && r.candidate_count > 1) r.notes.push_back("NOTE: " + std::to_string(r.candidate_count) + " candidate processes matched.");

    std::string json = BuildJson(r);
    {
        std::ofstream f(outPath, std::ios::binary);
        if (f) { f << json; f.flush(); }
    }

    if (!quiet) {
        std::printf("ark-env - PART 1: Environment Discovery & Process Identification (v1.1.0)\n");
        std::printf("  platform     : %s (elevated: %s, SeDebugPrivilege: %s)\n", r.os_string.c_str(), r.self_elevated ? "yes" : "no", r.se_debug ? "yes" : "no");
        if (r.game_found) {
            std::printf("  game process : %s (pid %lu)  [AppContainer: %s]\n", ToUtf8(r.proc_name).c_str(), (unsigned long)r.proc_pid, r.target_appcontainer ? "yes" : "no");
            std::printf("  image path   : %s\n", ToUtf8(r.proc_path).c_str());
            std::printf("  disambiguate : %s (candidates: %d)\n", ToUtf8(r.disambig_method).c_str(), r.candidate_count);
            std::printf("  engine module: %s  base=%s  size=%llu  version=%s\n", ToUtf8(r.engine_mod.name).c_str(), JHex(r.engine_mod.base).c_str(), (unsigned long long)r.engine_mod.size, r.engine_version_string.c_str());
            std::printf("  game module  : %s  base=%s  size=%llu  version=%s\n", ToUtf8(r.game_mod.name).c_str(), JHex(r.game_mod.base).c_str(), (unsigned long long)r.game_mod.size, r.ark_version_string.c_str());
            std::printf("  d3d          : %s (%s) version=%s\n", r.d3d_found ? ToUtf8(r.d3d_mod.name).c_str() : "not found", ToUtf8(r.d3d_flavor).c_str(), r.d3d_version_string.c_str());
            if (!r.appx.empty()) std::printf("  appx         : %s  version=%s\n", ToUtf8(r.appx[0].family).c_str(), ToUtf8(r.appx[0].version).c_str());
            std::printf("  window       : %s\n", r.window.found ? ("hwnd=" + JHexPtr(r.window.hwnd) + " class=" + ToUtf8(r.window.class_name) + " dpi=" + std::to_string((long)r.window.dpi) + " rect " + std::to_string((long)r.window.left) + "," + std::to_string((long)r.window.top) + " " + std::to_string((long)r.window.width) + "x" + std::to_string((long)r.window.height)).c_str() : "no window matched PID");
            if (r.game_monitor.found) std::printf("  game monitor : %s  %lux%lu @ %lu Hz\n", ToUtf8(r.game_monitor.device).c_str(), (long)r.game_monitor.pels_w, (long)r.game_monitor.pels_h, (long)r.game_monitor.refresh);
            std::printf("  display      : %dx%d primary; %zu monitor(s)\n", (long)r.primary_w, (long)r.primary_h, r.monitors.size());
            for (const auto& t : r.selftest.tiers) std::printf("  self-test    : %-52s %s (err %lu)\n", t.label.c_str(), t.ok ? "OK" : "FAIL", (unsigned long)t.err);
            std::printf("  self-test    : %s\n", ToUtf8(r.selftest.verdict).c_str());
        } else {
            std::printf("  game process : NOT FOUND\n");
        }
        std::printf("  report       : %s\n", outPath.c_str());
        std::printf("  status       : %s\n", r.status.c_str());
    }

    if (r.status == "FATAL") return 3;
    if (!r.game_found) return 2;
    if (r.selftest.read_ok && r.selftest.bytes_read >= 65536) return 0;
    return 1;
}