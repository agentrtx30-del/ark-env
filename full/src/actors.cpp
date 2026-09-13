#include "actors.h"

bool readActorArrayState(
    const WinMemory& mem,
    const RuntimeRoots& roots,
    const OffsetProfile& offsets,
    ActorArrayState& out
)
{
    out = {};

    uint64_t world = 0;
    if (!readPtr(mem, roots.gworldAddress, world))
        return false;

    uint64_t level = 0;
    if (!readPtr(mem, world + offsets.persistentLevelOffset, level))
        return false;

    const uint64_t arrayAddress = level + offsets.actorArrayOffset;

    uint64_t data = 0;
    if (!readPtr(mem, arrayAddress, data))
        return false;

    int32_t count = 0;
    if (!mem.read(arrayAddress + 8, count))
        return false;

    if (count < 0 || count > 200000)
        return false;

    out.valid = true;
    out.worldPtr = world;
    out.levelPtr = level;
    out.arrayAddress = arrayAddress;
    out.dataArray = data;
    out.count = count;

    return true;
}