#pragma once

#include "common.h"
#include "memory.h"
#include "offsets.h"
#include <string>
#include <unordered_map>
bool runFindCamera(const WinMemory& mem, const RuntimeRoots& roots, OffsetProfile& off,
                   const std::unordered_map<uint64_t, std::wstring>& idmap,
                   std::string& report);
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
void cameraResetRoute();