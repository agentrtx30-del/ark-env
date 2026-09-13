#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include "common.h"
#include "memory.h"
#include "offsets.h"

void runSelfCheck(
    const WinMemory& mem,
    const RuntimeRoots& roots,
    const OffsetProfile& offsets,
    int maxTargets,
    const std::unordered_map<uint64_t, std::wstring>* idmap = nullptr
);