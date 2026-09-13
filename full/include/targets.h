#pragma once

#include "common.h"
#include "memory.h"
#include "offsets.h"

bool readLogicalTarget(
    const WinMemory& mem,
    const OffsetProfile& offsets,
    uint64_t actor,
    LogicalTarget& out
);