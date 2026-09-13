#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "nametable.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

static std::wstring utf8ToWide(const char* s, int len)
{
    if (len <= 0)
        return {};

    const int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0)
        return {};

    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], n);
    return w;
}

int loadNameTable(
    const wchar_t* path,
    std::unordered_map<uint64_t, std::wstring>& out)
{
    out.clear();

    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (!f)
        return 0;

    char line[512];

    while (fgets(line, sizeof(line), f))
    {
        char* tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;

        char* name = tab + 1;
        size_t ln = strlen(name);
        while (ln > 0 && (name[ln - 1] == '\n' || name[ln - 1] == '\r'))
            name[--ln] = 0;
        if (ln == 0)
            continue;

        const uint64_t id = strtoull(line, nullptr, 10);
        out[id] = utf8ToWide(name, static_cast<int>(ln));
    }

    fclose(f);
    return static_cast<int>(out.size());
}

const wchar_t* resolveNameIndex(
    const std::unordered_map<uint64_t, std::wstring>& idmap,
    uint32_t index)
{
    auto it = idmap.find(static_cast<uint64_t>(index) * 2);
    if (it != idmap.end())
        return it->second.c_str();

    it = idmap.find(static_cast<uint64_t>(index) * 2 + 1);
    if (it != idmap.end())
        return it->second.c_str();

    return nullptr;
}