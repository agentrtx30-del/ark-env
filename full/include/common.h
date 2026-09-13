#pragma once
#include <cstdint>
#include "arkoverlay.h"

using Vec3f = arkoverlay::Vec3;

struct RuntimeRoots
{
    uint64_t shooterBase = 0;
    uint64_t gworldAddress = 0;
    uint64_t engineGlobalAddress = 0;
};

struct CameraState
{
    bool valid = false;
    int viewW = 0;
    int viewH = 0;
    float matrix16[16] = {};
    Vec3f cameraPos;
    Vec3f forward;
    Vec3f right;
    Vec3f up;
    float fovDegrees = 90.0f;
    bool projectionLike = false; // true = view-projection (screen path)
};

struct LogicalTarget
{
    uint64_t actorAddress = 0;
    Vec3f worldPos;
    float capsuleRadius = 0.0f;
    float halfHeight = 0.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    int level = -1;
    int ammo = -1;
    bool isPlayer = false;
    bool isTurret = false;
    bool isDino = false;
    bool isTamed = false;
    bool capsuleOk = false;

    // --- NEW (drift-proof position consensus) ---
    Vec3f directPos;        // actor + locationOffset (0xC00)
    Vec3f compPos;          // capsule/root component + componentLocationOffset (0xF0)
    bool posConsensus = false;

    wchar_t name[64] = {};
    wchar_t ownerName[64] = {};
    wchar_t classLabel[64] = {};
};

struct ActorArrayState
{
    bool valid = false;
    uint64_t worldPtr = 0;
    uint64_t levelPtr = 0;
    uint64_t arrayAddress = 0;
    uint64_t dataArray = 0;
    int32_t count = 0;
};