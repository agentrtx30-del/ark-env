#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include "memory.h"

enum class ClassKind { Unknown, Player, Dino, Pawn, Other };

struct ClassInfo
{
    ClassKind kind = ClassKind::Unknown;
    std::wstring label;      // e.g. "Triceratops"
    std::wstring chainText;
    bool resolved = false;
};

ClassInfo classifyActor(
    const WinMemory& mem,
    const std::unordered_map<uint64_t, std::wstring>& idmap,
    uint64_t actor,
    uint32_t classOffset = 0x10,
    uint32_t superOffset = 0x30,
    int depth = 12
);

// Debug helper: raw super-chain with resolved names (unresolved shown as id_N).
std::wstring debugChain(
    const WinMemory& mem,
    const std::unordered_map<uint64_t, std::wstring>& idmap,
    uint64_t actor,
    int depth = 8
);