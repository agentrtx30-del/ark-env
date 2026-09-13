#include "selfcheck.h"
#include "log.h"
#include "camera.h"
#include "actors.h"
#include "targets.h"
#include "classifier.h"
#include <cmath>
#include <cstdio>

void runSelfCheck(
    const WinMemory& mem,
    const RuntimeRoots& roots,
    const OffsetProfile& offsets,
    int maxTargets,
    const std::unordered_map<uint64_t, std::wstring>* idmap)
{
    printf("\n=== ARK-FULL SELF CHECK ===\n");
    printf("[roots]\n");
    printf("  ShooterGame.exe base = 0x%016llX\n", roots.shooterBase);
    printf("  gworldAddress        = 0x%016llX\n", roots.gworldAddress);
    printf("  engineGlobalAddress  = 0x%016llX\n", roots.engineGlobalAddress);

    // ---- Anchor: prefer a live pawn (hp>0), else any capsule-bearing pawn.
    float anchor[3] = { 0.0f, 0.0f, 0.0f };
    bool anchorValid = false;
    ActorArrayState actors;
    const bool actorsOk = readActorArrayState(mem, roots, offsets, actors);
    if (actorsOk)
    {
        int bestTier = -1;
        float bx = 0.0f, by = 0.0f, bz = 0.0f;
        for (int i = 0; i < actors.count && bestTier < 2; ++i)
        {
            uint64_t ap = 0;
            if (!readPtr(mem, actors.dataArray + static_cast<uint64_t>(i) * 8, ap))
                continue;
            LogicalTarget lt;
            if (!readLogicalTarget(mem, offsets, ap, lt))
                continue;
            const float ax = fabsf(lt.worldPos.x);
            const float ay = fabsf(lt.worldPos.y);
            const float az = fabsf(lt.worldPos.z);
            if (ax > 5.0e6f || ay > 5.0e6f || az > 5.0e6f)
                continue;
            if ((ax + ay + az) <= 10000.0f)
                continue;
            int tier = -1;
            if (lt.health > 0.0f && lt.maxHealth > 0.0f)
                tier = 2;
            else if (lt.capsuleOk)
                tier = 1;
            if (tier <= bestTier)
                continue;
            bestTier = tier;
            bx = lt.worldPos.x;
            by = lt.worldPos.y;
            bz = lt.worldPos.z;
        }
        if (bestTier >= 1)
        {
            anchor[0] = bx;
            anchor[1] = by;
            anchor[2] = bz;
            anchorValid = true;
        }
    }
    printf("[anchor]\n");
    if (anchorValid)
        printf("  pos = %.1f %.1f %.1f\n", anchor[0], anchor[1], anchor[2]);
    else
        printf("  (none found)\n");

    cameraDebugSetVerbose(true);
    CameraState camera;
    const bool camOk = readCameraState(mem, roots, offsets, camera,
                                       1280, 720, anchorValid ? anchor : nullptr);
    cameraDebugSetVerbose(false);
    if (!camOk)
    {
        printf("[camera]\nFAILED - candidate hunt results:\n");
        cameraDebugDumpCandidates();
        printf("\n");
    }
    else
    {
        printf("[camera]\n");
        printf("  resolution = %dx%d\n", camera.viewW, camera.viewH);
        printf("  pos        = %.1f %.1f %.1f\n",
               camera.cameraPos.x, camera.cameraPos.y, camera.cameraPos.z);
    }

    if (!actorsOk)
    {
        printf("[actors]\nFAILED\n");
        return;
    }
    printf("[actors]\n");
    printf("  world = 0x%016llX\n", actors.worldPtr);
    printf("  level = 0x%016llX\n", actors.levelPtr);
    printf("  array = 0x%016llX\n", actors.arrayAddress);
    printf("  data  = 0x%016llX\n", actors.dataArray);
    printf("  count = %d\n", actors.count);

    printf("[pawn-like]\n");
    int printed = 0;
    int chainDumped = 0;
    for (int i = 0; i < actors.count && printed < maxTargets; ++i)
    {
        uint64_t actorPtr = 0;
        const uint64_t addr =
            actors.dataArray + static_cast<uint64_t>(i) * sizeof(uint64_t);
        if (!readPtr(mem, addr, actorPtr))
            continue;
        LogicalTarget t;
        if (!readLogicalTarget(mem, offsets, actorPtr, t))
            continue;
        const float ax = fabsf(t.worldPos.x);
        const float ay = fabsf(t.worldPos.y);
        const float az = fabsf(t.worldPos.z);
        if (ax > 5.0e6f || ay > 5.0e6f || az > 5.0e6f)
            continue;
        if ((ax + ay + az) <= 10000.0f)
            continue;
        ++printed;
        printf(
            "  %03d actor=0x%016llX pos=(%.1f, %.1f, %.1f) cons=%d capsule=%d\n"
            "      direct=(%.1f, %.1f, %.1f)\n"
            "      comp  =(%.1f, %.1f, %.1f)\n"
            "      hp=%.1f/%.1f lvl=%d r=%.1f hh=%.1f tamed=%d\n",
            printed,
            actorPtr,
            t.worldPos.x, t.worldPos.y, t.worldPos.z,
            t.posConsensus ? 1 : 0,
            t.capsuleOk ? 1 : 0,
            t.directPos.x, t.directPos.y, t.directPos.z,
            t.compPos.x, t.compPos.y, t.compPos.z,
            t.health, t.maxHealth,
            t.level,
            t.capsuleRadius, t.halfHeight,
            t.isTamed ? 1 : 0);
        // Dump chains for capsule-bearing actors (the real pawns).
        if (idmap && !idmap->empty() && t.capsuleOk && chainDumped < 6)
        {
            ++chainDumped;
            printf("      chain: %ls\n",
                   debugChain(mem, *idmap, actorPtr).c_str());
        }
    }
    if (printed == 0)
        printf("  (none)\n");

    printf("\n[summary]\npawnLike=%d cam=%d\n", printed, camOk ? 1 : 0);
    logInfo("self-check complete: pawnLike=%d cam=%d", printed, camOk ? 1 : 0);
}