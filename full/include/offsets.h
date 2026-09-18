#pragma once
#include <cstdint>
#include <string>

struct OffsetProfile
{
    bool valid = false;
    std::wstring gameVersion;
    std::wstring engineVersion;
    std::wstring packageFullName;
    std::wstring engineModule;
    std::wstring gameModule;
    uint64_t gworldRva = 0;
    uint64_t engineGlobalRva = 0;
    uint32_t gameInstanceOffset = 0;
    uint32_t localPlayersOffset = 0;
    uint32_t viewportClientOffset = 0;
    uint32_t viewportOffset = 0;
    uint32_t resolutionOffset = 0;
    uint32_t viewMatrixOffset = 0;
    uint32_t persistentLevelOffset = 0;
    uint32_t actorArrayOffset = 0;
    uint32_t locationOffset = 0;
    uint32_t capsuleOffset = 0;
    uint32_t capsuleRadiusOffset = 0;
    uint32_t capsuleHalfHeightOffset = 0;
    uint32_t healthOffset = 0;
    uint32_t maxHealthOffset = 0;
    uint32_t statusComponentOffset = 0;
    uint32_t levelOffset = 0;
    uint32_t tamedBoolOffset = 0;
    uint32_t backupTamedBoolOffset = 0;
    uint32_t teamIdOffset = 0;
    uint32_t altGameInstanceOffset = 0;
    uint32_t altLocalPlayersOffset = 0;
    uint32_t altResolutionDirect = 0;
    uint32_t altResolutionViewportOffset = 0;
    uint32_t altResolutionOffset = 0;
    uint32_t playerControllerOffset = 0;   // ULocalPlayer -> APlayerController
    uint32_t pawnOffset = 0;               // AController -> APawn
    uint32_t pcmOffset = 0;                // v30: APlayerController -> APlayerCameraManager
    uint32_t povOffset = 0;                // v30: PCM -> FMinimalViewInfo (Loc+Rot+FOV, 28 B)
    std::wstring expectedPackageVersion;
    uint64_t expectedModuleSize = 0;
};

OffsetProfile makeDefaultOffsetProfile();