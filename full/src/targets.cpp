#include "targets.h"
#include <cmath>
#include <cwchar>

static bool finiteVec3(const Vec3f& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool plausibleVec3(const Vec3f& v)
{
    if (!finiteVec3(v)) return false;
    if (fabsf(v.x) > 1.0e8f || fabsf(v.y) > 1.0e8f || fabsf(v.z) > 1.0e8f) return false;
    return (fabsf(v.x) + fabsf(v.y) + fabsf(v.z)) > 1.0f;
}

bool readLogicalTarget(
    const WinMemory& mem,
    const OffsetProfile& offsets,
    uint64_t actor,
    LogicalTarget& out)
{
    out = {};
    out.actorAddress = actor;

    // ---- Location: validated component read (authoritative).
    // A component is trusted only if it carries a plausible CapsuleRadius at
    // +0x124; this rejects jittering mesh components. While we have the
    // validated component open, also capture its radius/half-height so box
    // sizes stay correct even when offsets.capsuleOffset (0x268) is dead.
    Vec3f pos{};
    bool havePos = false;
    float valRadius = 0.0f, valHalf = 0.0f;
    bool valOk = false;
    int valSrc = -1;

    const uint32_t compOffsets[2] = { offsets.capsuleOffset, 0x170u };
    for (int k = 0; k < 2 && !havePos; ++k)
    {
        uint64_t comp = 0;
        if (!readPtr(mem, actor + compOffsets[k], comp)) continue;

        float radius = 0.0f;
        if (!mem.read(comp + 0x124, radius)) continue;
        if (!std::isfinite(radius) || radius <= 0.0f || radius > 1500.0f) continue;

        float half = 0.0f;
        const bool haveHalf = mem.read(comp + 0x12c, half) &&
                              std::isfinite(half) &&
                              half > 0.0f && half < 3000.0f;

        Vec3f cpos;
        if (!mem.read(comp + 0xF0, cpos)) continue;   // componentLocationOffset
        if (!plausibleVec3(cpos)) continue;

        pos = cpos;
        havePos = true;
        if (haveHalf && !valOk)
        {
            valRadius = radius;
            valHalf = half;
            valOk = true;
            valSrc = k;
        }
    }
    // NO fallback to actor+0xC00 (stale/zero in this build => flicker).

    if (!havePos)
        return false;
    out.worldPos = pos;   // capsule CENTER (tracker space); main converts to base

    // ---- Capsule: prefer explicit capsule-component read, else the values
    //      captured from the validated component above, else default box.
    uint64_t capsule = 0;
    float radius = 0.0f, halfHeight = 0.0f;
    if (readPtr(mem, actor + offsets.capsuleOffset, capsule) &&
        mem.read(capsule + offsets.capsuleRadiusOffset, radius) &&
        mem.read(capsule + offsets.capsuleHalfHeightOffset, halfHeight) &&
        std::isfinite(radius) && std::isfinite(halfHeight) &&
        radius > 0.0f && radius < 1500.0f &&
        halfHeight > 0.0f && halfHeight < 3000.0f)
    {
        out.capsuleRadius = radius;
        out.halfHeight = halfHeight;
        out.capsuleOk = true;
    }
    else if (valOk)
    {
        out.capsuleRadius = valRadius;
        out.halfHeight = valHalf;
        out.capsuleOk = true;
    }
    else
    {
        out.capsuleRadius = 50.0f;
        out.halfHeight = 100.0f;
        out.capsuleOk = false;
    }

    // ---- Health: optional.
    float health = 0.0f, maxHealth = 0.0f;
    if (mem.read(actor + offsets.healthOffset, health) &&
        mem.read(actor + offsets.maxHealthOffset, maxHealth) &&
        std::isfinite(health) && std::isfinite(maxHealth) &&
        maxHealth >= 0.0f && health >= -1.0f && health <= 1.0e6f)
    {
        out.health = health;
        out.maxHealth = maxHealth;
    }

    // ---- Level: optional.
    uint64_t statusComponent = 0;
    if (readPtr(mem, actor + offsets.statusComponentOffset, statusComponent))
    {
        int32_t level = -1;
        if (mem.read(statusComponent + offsets.levelOffset, level))
            if (level >= 0 && level < 1000000) out.level = level;
    }

    // ---- Tamed flag: optional.
    uint8_t tamed = 0;
    if (mem.read(actor + offsets.tamedBoolOffset, tamed))
        out.isTamed = (tamed != 0);
    else if (offsets.backupTamedBoolOffset != 0 &&
             mem.read(actor + offsets.backupTamedBoolOffset, tamed))
        out.isTamed = (tamed != 0);

    out.isDino = true;
    out.isPlayer = false;
    out.isTurret = false;
    wcscpy_s(out.classLabel, L"Target");
    wcscpy_s(out.name, L"Target");
    return true;
}