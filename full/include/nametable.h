#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Loads work/idmap.tsv (produced by scripts/ark_part6_dump_idmap.py).
int loadNameTable(
    const wchar_t* path,
    std::unordered_map<uint64_t, std::wstring>& out
);

// Resolves the name index stored at [classPtr + 0x18].
const wchar_t* resolveNameIndex(
    const std::unordered_map<uint64_t, std::wstring>& idmap,
    uint32_t index
);