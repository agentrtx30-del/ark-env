#include "offsets.h"

OffsetProfile makeDefaultOffsetProfile()
{
    OffsetProfile p;
    p.valid = true;
    p.gameVersion = L"1.212.962.2";
    p.engineVersion = L"1.212.962.2";
    p.packageFullName =
        L"StudioWildcard.4558480580BB9_1.212.962.2_x64__1w2mm55455e38";
    p.engineModule = L"ShooterGame.exe";
    p.gameModule = L"ShooterGame.exe";

    // --- NEW: version drift gate ---
    p.expectedPackageVersion = L"1.212.962.2";
    p.expectedModuleSize = 0; // fill from ark_env.json modules.game.size_bytes if wanted

    // Root RVAs.
    p.gworldRva = 0x43a5758ULL;
    p.engineGlobalRva = 0x438e270ULL;

    // Camera chain.
    p.gameInstanceOffset = 0x5f48;
    p.localPlayersOffset = 0xf0;
    // Live-discovered 2026-09-05 (session 11084): real UGameViewportClient*.
    // 0x208 is dead in this build; probe remains as fallback.
    p.viewportClientOffset = 0x1e0;
    p.viewportOffset = 0x20;
    p.resolutionOffset = 0x38;
    p.viewMatrixOffset = 0x60;

    // Actor array.
    p.persistentLevelOffset = 0xf8;
    p.actorArrayOffset = 0x88;

    // Actor / pawn fields.
    p.locationOffset = 0xc00;
    p.capsuleOffset = 0x268;
    p.capsuleRadiusOffset = 0x124;
    p.capsuleHalfHeightOffset = 0x12c;
    p.healthOffset = 0x964;
    p.maxHealthOffset = 0x968;
    p.statusComponentOffset = 0xd10;
    p.levelOffset = 0x6cc;
    p.tamedBoolOffset = 0x14c0;
    p.backupTamedBoolOffset = 0x14d0;
    p.teamIdOffset = 0x148c;

    // Alternatives from ark_offsets.json.
    p.altGameInstanceOffset = 0x5ce8;
    p.altLocalPlayersOffset = 0x320;
    p.altResolutionDirect = 0x191c;
    p.altResolutionViewportOffset = 0x18;
    p.altResolutionOffset = 0x7e8;
    return p;
}