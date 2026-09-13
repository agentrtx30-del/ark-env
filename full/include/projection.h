#pragma once

#include "common.h"
#include "arkoverlay.h"

float distanceCm(const Vec3f& a, const Vec3f& b);
float distanceMeters(const Vec3f& a, const Vec3f& b);

bool makeOverlayCamera(const CameraState& camera, arkoverlay::Camera& out);

bool projectPoint(
    const float m[16], int viewW, int viewH,
    const Vec3f& p, float& sx, float& sy, float& w
);

bool screenBoxFromMatrix(
    const float m[16], int viewW, int viewH,
    const Vec3f& pos, float radius, float halfHeight,
    float& sx, float& sy, float& boxW, float& boxH
);