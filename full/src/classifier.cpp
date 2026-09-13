#include "classifier.h"
#include "nametable.h"

#include <vector>

static bool containsW(const std::wstring& hay, const char* needle)
{
    std::wstring w;
    for (const char* p = needle; *p; ++p)
        w.push_back(static_cast<wchar_t>(*p));
    return hay.find(w) != std::wstring::npos;
}

ClassInfo classifyActor(
    const WinMemory& mem,
    const std::unordered_map<uint64_t, std::wstring>& idmap,
    uint64_t actor,
    uint32_t classOffset,
    uint32_t superOffset,
    int depth)
{
    ClassInfo info;

    uint64_t cp = 0;
    if (!readPtr(mem, actor + classOffset, cp))
        return info;

    std::wstring joined;
    std::wstring first;
    std::vector<uint64_t> seen;

    for (int i = 0; i < depth; ++i)
    {
        if (!cp)
            break;

        bool dup = false;
        for (uint64_t s : seen)
            if (s == cp) { dup = true; break; }
        if (dup)
            break;
        seen.push_back(cp);

        uint64_t nameRaw = 0;
        if (!mem.read(cp + 0x18, nameRaw))
            break;

        const uint32_t idx = static_cast<uint32_t>(nameRaw & 0xFFFFFFFF);
        const wchar_t* nm = resolveNameIndex(idmap, idx);

        if (nm && nm[0])
        {
            if (first.empty())
                first = nm;
            if (!joined.empty())
                joined += L"|";
            joined += nm;
            info.resolved = true;
        }

        uint64_t next = 0;
        if (!readPtr(mem, cp + superOffset, next))
            break;
        cp = next;
    }

    info.chainText = joined;

    if (containsW(joined, "PrimalDinoCharacter") ||
        containsW(joined, "DinoCharacter") ||
        containsW(joined, "PrimalCharacter"))
    {
        info.kind = ClassKind::Dino;
    }
    else if (containsW(joined, "ShooterCharacter") ||
        containsW(joined, "PlayerController") ||
        containsW(joined, "PlayerState"))
    {
        info.kind = ClassKind::Player;
    }
    else if (containsW(joined, "Pawn"))
    {
        info.kind = ClassKind::Pawn;
    }
    else
    {
        info.kind = ClassKind::Other;
    }

    // Label: first chain entry with BP suffixes stripped.
    std::wstring label = first;
    const wchar_t* suffixes[] = { L"_Character_C", L"_Character", L"_C" };
    for (const wchar_t* s : suffixes)
    {
        const size_t p = label.find(s);
        if (p != std::wstring::npos)
        {
            label = label.substr(0, p);
            break;
        }
    }
    info.label = label;

    return info;
}

std::wstring debugChain(
    const WinMemory& mem,
    const std::unordered_map<uint64_t, std::wstring>& idmap,
    uint64_t actor,
    int depth)
{
    std::wstring out;

    uint64_t cp = 0;
    if (!readPtr(mem, actor + 0x10, cp))
        return L"(no class pointer)";

    for (int i = 0; i < depth && cp; ++i)
    {
        uint64_t nameRaw = 0;
        if (!mem.read(cp + 0x18, nameRaw))
        {
            if (!out.empty()) out += L"|";
            out += L"!readname";
            break;
        }

        const uint32_t idx = static_cast<uint32_t>(nameRaw & 0xFFFFFFFF);
        const wchar_t* nm = resolveNameIndex(idmap, idx);

        if (!out.empty())
            out += L"|";

        if (nm && nm[0])
            out += nm;
        else
        {
            out += L"id_";
            out += std::to_wstring(idx);
        }

        uint64_t next = 0;
        if (!readPtr(mem, cp + 0x30, next))
            break;
        cp = next;
    }

    return out;
}