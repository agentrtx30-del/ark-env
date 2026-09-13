#include "projection.h"

#include <cmath>
#include <algorithm>

float distanceCm(const Vec3f& a, const Vec3f& b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

float distanceMeters(const Vec3f& a, const Vec3f& b)
{
    return distanceCm(a, b) / 100.0f;
}

static Vec3f normalize(Vec3f v)
{
    const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f)
        return { 0.0f, 0.0f, 0.0f };
    return { v.x / len, v.y / len, v.z / len };
}

bool makeOverlayCamera(const CameraState& camera, arkoverlay::Camera& out)
{
    if (!camera.valid)
        return false;

    out = {};
    out.pos = camera.cameraPos;
    out.forward = normalize(camera.forward);
    out.right = normalize(camera.right);
    out.up = normalize(camera.up);
    out.fovDegrees = camera.fovDegrees;
    out.screenW = camera.viewW;
    out.screenH = camera.viewH;
    out.playerPos = camera.cameraPos;
    return true;
}

bool projectPoint(
    const float m[16], int viewW, int viewH,
    const Vec3f& p, float& sx, float& sy, float& w
)
{
    // Row-vector convention, identical to arkmath.py project().
    const float cx = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
    const float cy = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
    const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];

    if (!(cw > 1e-6f))
        return false;

    const float nx = cx / cw;
    const float ny = cy / cw;

    sx = (nx + 1.0f) * 0.5f * static_cast<float>(viewW);
    sy = (1.0f - ny) * 0.5f * static_cast<float>(viewH);
    w = cw;

    return std::isfinite(sx) && std::isfinite(sy);
}

bool screenBoxFromMatrix(
    const float m[16], int viewW, int viewH,
    const Vec3f& pos, float radius, float halfHeight,
    float& sx, float& sy, float& boxW, float& boxH
)
{
    float cx, cy, cw;
    float tx, ty, tw;
    float bx, by, bw;
    float lx, ly, lw;
    float rx, ry, rw;

    const Vec3f top    = { pos.x, pos.y, pos.z + halfHeight };
    const Vec3f bottom = { pos.x, pos.y, pos.z - halfHeight };
    const Vec3f left   = { pos.x - radius, pos.y, pos.z };
    const Vec3f right  = { pos.x + radius, pos.y, pos.z };

    if (!projectPoint(m, viewW, viewH, pos, cx, cy, cw)) return false;
    if (!projectPoint(m, viewW, viewH, top, tx, ty, tw)) return false;
    if (!projectPoint(m, viewW, viewH, bottom, bx, by, bw)) return false;
    if (!projectPoint(m, viewW, viewH, left, lx, ly, lw)) return false;
    if (!projectPoint(m, viewW, viewH, right, rx, ry, rw)) return false;

    boxH = fabsf(by - ty);
    boxW = fabsf(rx - lx);

    const float maxDim = static_cast<float>(std::max(viewW, viewH)) * 2.0f;
    boxW = std::clamp(boxW, 1.0f, maxDim);
    boxH = std::clamp(boxH, 1.0f, maxDim);

    sx = cx;
    sy = cy;
    return true;
}