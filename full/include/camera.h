#pragma once

#include "common.h"
#include "memory.h"
#include "offsets.h"

// anchor: optional 3-float world position of a known live pawn; used to pick
// the matrix whose camera sits next to it.
bool readCameraState(
    const WinMemory& mem,
    const RuntimeRoots& roots,
    const OffsetProfile& offsets,
    CameraState& out,
    int fallbackW = 0,
    int fallbackH = 0,
    const float* anchor = nullptr
);

void cameraDebugSetVerbose(bool verbose);
void cameraDebugDumpCandidates();