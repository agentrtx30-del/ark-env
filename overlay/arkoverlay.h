#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <cstdint>

namespace arkoverlay {

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Camera
{
    Vec3 pos;
    Vec3 forward{ 0.0f, 0.0f, 1.0f };
    Vec3 right{ 1.0f, 0.0f, 0.0f };
    Vec3 up{ 0.0f, 1.0f, 0.0f };
    float fovDegrees = 90.0f;
    int screenW = 1280;
    int screenH = 720;
    Vec3 playerPos;
};

struct Target
{
    wchar_t name[64] = {};
    wchar_t ownerName[64] = {};
    bool isPlayer = false;
    bool isTurret = false;
    bool behindCamera = false;
    Vec3 worldPos;
    Vec3 headPos;
    float boxW = 0.6f;
    float boxH = 1.8f;
    float health = 100.0f;
    float maxHealth = 100.0f;
    float distance = 0.0f;
    int level = -1;
    int ammo = -1;
};

// Pre-projected (screen-space) target for the matrix draw path.
struct ScreenTarget
{
    wchar_t name[64] = {};
    wchar_t ownerName[64] = {};
    bool isPlayer = false;
    bool isTurret = false;
    float screenX = 0.0f;
    float screenY = 0.0f;
    float boxW = 0.0f;
    float boxH = 0.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float distance = 0.0f;
    int level = -1;
    int ammo = -1;
};

struct Config
{
    bool enableBoxes = true;
    bool enableNames = true;
    bool enableHealth = true;
    bool enableDistance = true;
    bool playersOnly = false;
    bool showFps = true;
    bool showLevel = true;
    bool showCoords = true;
    bool showAmmo = true;
    bool enableTurrets = true;
    float maxRange = 100000.0f;
    float widthFactor = 1.0f;
    float fontScale = 1.0f;
    uint32_t playerColor = 0xFFFF64C8;
    uint32_t dinoColor = 0xFFFF2828;
    uint32_t healthBarColor = 0xFF45E045;
    uint32_t turretColor = 0xFFFFC000;
};

bool overlayInit(const RECT& windowRect);
bool overlayDraw(
    const Target* targetList,
    int targetCount,
    const Camera& camera,
    const Config& config
);
bool overlayDrawScreen(
    const ScreenTarget* targetList,
    int targetCount,
    int screenW,
    int screenH,
    const Vec3& playerPos,
    const Config& config
);
void overlayHide();
void overlayShow();
void overlayResize(const RECT& newRect);
bool overlayIsInitialized();
void overlayShutdown();
void loadConfig(const wchar_t* iniPath, Config& out);
void saveConfig(const wchar_t* iniPath, const Config& cfg);
bool overlayDemo(const RECT* initialRect = nullptr);

} // namespace arkoverlay