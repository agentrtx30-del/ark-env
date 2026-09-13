"""
arkmath: pure projection and box math for ARK rendering.

No game dependencies. 100% offline capable.

Frozen Camera layout:
{
    "matrix16": [16 floats],  # 4x4 view-projection matrix
    "viewW": int,             # Viewport width in pixels
    "viewH": int,             # Viewport height in pixels
}

Frozen Target layout:
{
    "worldX": float,
    "worldY": float,
    "worldZ": float,
    "halfHeight": float,
    "health": float,
    "maxHealth": float,
    "name": str,
    "isPlayer": bool,
    "classLabel": str,
    "distance": float,
    "screenX": float,
    "screenY": float,
    "boxW": float,
    "boxH": float,
    "behindCamera": bool,
}
"""
import math

# Epsilon to prevent division by zero and handle behind-camera culling
EPSILON = 1e-6


def distance(camX, camY, camZ, worldX, worldY, worldZ):
    """Calculate 3D Euclidean distance between camera and target."""
    dx = worldX - camX
    dy = worldY - camY
    dz = worldZ - camZ
    return math.sqrt(dx * dx + dy * dy + dz * dz)


def project(matrix16, viewW, viewH, worldX, worldY, worldZ, row_vector=True):
    """
    Project a 3D world point to 2D screen coordinates.
    
    Returns:
        tuple: (screenX, screenY, w, behindCamera)
    """
    if len(matrix16) != 16:
        raise ValueError("matrix16 must contain exactly 16 floats")
    if viewW <= 0 or viewH <= 0:
        raise ValueError("viewW and viewH must be positive")

    # 1. Matrix multiplication
    if row_vector:
        # Row-vector convention: [x, y, z, 1] * M
        clipX = (matrix16[0] * worldX + matrix16[4] * worldY + 
                 matrix16[8] * worldZ + matrix16[12])
        clipY = (matrix16[1] * worldX + matrix16[5] * worldY + 
                 matrix16[9] * worldZ + matrix16[13])
        clipZ = (matrix16[2] * worldX + matrix16[6] * worldY + 
                 matrix16[10] * worldZ + matrix16[14])
        clipW = (matrix16[3] * worldX + matrix16[7] * worldY + 
                 matrix16[11] * worldZ + matrix16[15])
    else:
        # Column-vector convention: M * [x, y, z, 1]^T
        clipX = (matrix16[0] * worldX + matrix16[1] * worldY + 
                 matrix16[2] * worldZ + matrix16[3])
        clipY = (matrix16[4] * worldX + matrix16[5] * worldY + 
                 matrix16[6] * worldZ + matrix16[7])
        clipZ = (matrix16[8] * worldX + matrix16[9] * worldY + 
                 matrix16[10] * worldZ + matrix16[11])
        clipW = (matrix16[12] * worldX + matrix16[13] * worldY + 
                 matrix16[14] * worldZ + matrix16[15])

    # 2. Behind-camera culling
    behindCamera = clipW <= EPSILON
    if behindCamera:
        return 0.0, 0.0, clipW, True

    # 3. Perspective divide (NDC)
    ndcX = clipX / clipW
    ndcY = clipY / clipW

    # 4. NDC to Screen mapping (with Y-flip so +Y world up = smaller screen Y)
    screenX = (ndcX + 1.0) * 0.5 * viewW
    screenY = (1.0 - ndcY) * 0.5 * viewH

    return screenX, screenY, clipW, False


def boxFor(camera, worldX, worldY, worldZ, halfHeight, widthFactor):
    """
    Calculate screen-space bounding box and center line for a target.
    
    Returns:
        dict: Contains behindCamera, screenX/Y, boxW/H, centerLine, and corners.
    """
    matrix16 = camera["matrix16"]
    viewW = camera["viewW"]
    viewH = camera["viewH"]

    # Project center, top, and bottom points
    centerX, centerY, centerW, centerBehind = project(
        matrix16, viewW, viewH, worldX, worldY, worldZ
    )
    topX, topY, topW, topBehind = project(
        matrix16, viewW, viewH, worldX, worldY + halfHeight, worldZ
    )
    bottomX, bottomY, bottomW, bottomBehind = project(
        matrix16, viewW, viewH, worldX, worldY - halfHeight, worldZ
    )

    # If any critical point is behind the camera, discard the box
    behindCamera = centerBehind or topBehind or bottomBehind

    if behindCamera:
        return {
            "behindCamera": True,
            "screenX": 0.0, "screenY": 0.0, "w": centerW,
            "boxW": 0.0, "boxH": 0.0,
            "centerLine": {"top": {"x": 0.0, "y": 0.0}, "bottom": {"x": 0.0, "y": 0.0}},
            "corners": {
                "topLeft": {"x": 0.0, "y": 0.0}, "topRight": {"x": 0.0, "y": 0.0},
                "bottomRight": {"x": 0.0, "y": 0.0}, "bottomLeft": {"x": 0.0, "y": 0.0},
            },
        }

    # Calculate projected height
    boxH = abs(bottomY - topY)
    
    # Calculate and clamp projected width
    rawBoxW = widthFactor / max(centerW, EPSILON)
    minBoxW = 1.0
    maxBoxW = viewW * 0.5
    boxW = max(minBoxW, min(rawBoxW, maxBoxW))

    # Build corners based on center X and top/bottom Y
    halfW = boxW * 0.5
    left = centerX - halfW
    right = centerX + halfW
    top = topY
    bottom = bottomY

    return {
        "behindCamera": False,
        "screenX": centerX,
        "screenY": centerY,
        "w": centerW,
        "boxW": boxW,
        "boxH": boxH,
        "centerLine": {
            "top": {"x": topX, "y": topY},
            "bottom": {"x": bottomX, "y": bottomY},
        },
        "corners": {
            "topLeft": {"x": left, "y": top},
            "topRight": {"x": right, "y": top},
            "bottomRight": {"x": right, "y": bottom},
            "bottomLeft": {"x": left, "y": bottom},
        },
    }