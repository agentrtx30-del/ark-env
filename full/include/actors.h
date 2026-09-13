#pragma once

#include "common.h"
#include "memory.h"
#include "offsets.h"

bool readActorArrayState(
    const WinMemory& mem,
    const RuntimeRoots& roots,
    const OffsetProfile& offsets,
    ActorArrayState& out
);