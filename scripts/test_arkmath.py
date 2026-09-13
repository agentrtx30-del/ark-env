import unittest
import sys
import os

# Ensure the scripts directory is in the path so we can import arkmath
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arkmath

class TestArkMath(unittest.TestCase):
    def setUp(self):
        # Synthetic matrix for row-vector convention: [x, y, z, 1] * M
        # Math: clipX = x, clipY = y, clipW = -z
        # This means a point at Z = -1 is in front of the camera (clipW = 1)
        self.TEST_MATRIX = [
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 0.0, -1.0,
            0.0, 0.0, 0.0, 0.0,
        ]
        self.VIEW_W = 100.0
        self.VIEW_H = 100.0
        self.camera = {
            "matrix16": self.TEST_MATRIX,
            "viewW": self.VIEW_W,
            "viewH": self.VIEW_H,
        }

    def test_distance(self):
        self.assertAlmostEqual(arkmath.distance(0, 0, 0, 3, 4, 0), 5.0)
        self.assertAlmostEqual(arkmath.distance(1, 1, 1, 1, 1, 1), 0.0)

    def test_project_center(self):
        """Point dead center (0,0,-1) should map to screen center (50,50)."""
        screenX, screenY, w, behind = arkmath.project(
            self.TEST_MATRIX, self.VIEW_W, self.VIEW_H, 0.0, 0.0, -1.0
        )
        self.assertAlmostEqual(screenX, 50.0)
        self.assertAlmostEqual(screenY, 50.0)
        self.assertAlmostEqual(w, 1.0)
        self.assertFalse(behind)

    def test_project_right_edge(self):
        """Point far right (1,0,-1) should map to right edge (100,50)."""
        screenX, screenY, w, behind = arkmath.project(
            self.TEST_MATRIX, self.VIEW_W, self.VIEW_H, 1.0, 0.0, -1.0
        )
        self.assertAlmostEqual(screenX, 100.0)
        self.assertAlmostEqual(screenY, 50.0)
        self.assertFalse(behind)

    def test_project_behind_camera(self):
        """Point behind camera (0,0,1) should be culled."""
        screenX, screenY, w, behind = arkmath.project(
            self.TEST_MATRIX, self.VIEW_W, self.VIEW_H, 0.0, 0.0, 1.0
        )
        self.assertTrue(behind)
        self.assertLessEqual(w, arkmath.EPSILON)

    def test_project_y_flip(self):
        """World UP (+Y) should map to Screen UP (smaller Y)."""
        _, screenY_up, _, _ = arkmath.project(
            self.TEST_MATRIX, self.VIEW_W, self.VIEW_H, 0.0, 1.0, -1.0
        )
        self.assertAlmostEqual(screenY_up, 0.0)  # Top of screen

        _, screenY_down, _, _ = arkmath.project(
            self.TEST_MATRIX, self.VIEW_W, self.VIEW_H, 0.0, -1.0, -1.0
        )
        self.assertAlmostEqual(screenY_down, 100.0)  # Bottom of screen

    def test_boxFor_near_vs_far(self):
        """Near targets should get wider boxes than far targets."""
        # Near target (z=-1, w=1) -> widthFactor(10) / 1 = 10
        boxNear = arkmath.boxFor(self.camera, 0.0, 0.0, -1.0, 0.5, 10.0)
        self.assertAlmostEqual(boxNear["boxW"], 10.0)
        
        # Far target (z=-2, w=2) -> widthFactor(10) / 2 = 5
        boxFar = arkmath.boxFor(self.camera, 0.0, 0.0, -2.0, 0.5, 10.0)
        self.assertAlmostEqual(boxFar["boxW"], 5.0)

    def test_boxFor_corners_and_centerline(self):
        """Verify exact box geometry and center bone line."""
        box = arkmath.boxFor(self.camera, 0.0, 0.0, -1.0, 0.5, 10.0)
        self.assertFalse(box["behindCamera"])
        self.assertAlmostEqual(box["boxW"], 10.0)
        self.assertAlmostEqual(box["boxH"], 50.0) # topY=25, bottomY=75 -> diff=50

        # Center line checks
        self.assertAlmostEqual(box["centerLine"]["top"]["x"], 50.0)
        self.assertAlmostEqual(box["centerLine"]["top"]["y"], 25.0)
        self.assertAlmostEqual(box["centerLine"]["bottom"]["x"], 50.0)
        self.assertAlmostEqual(box["centerLine"]["bottom"]["y"], 75.0)

        # Corner checks
        self.assertAlmostEqual(box["corners"]["topLeft"]["x"], 45.0)
        self.assertAlmostEqual(box["corners"]["topLeft"]["y"], 25.0)
        self.assertAlmostEqual(box["corners"]["topRight"]["x"], 55.0)
        self.assertAlmostEqual(box["corners"]["topRight"]["y"], 25.0)
        self.assertAlmostEqual(box["corners"]["bottomRight"]["x"], 55.0)
        self.assertAlmostEqual(box["corners"]["bottomRight"]["y"], 75.0)
        self.assertAlmostEqual(box["corners"]["bottomLeft"]["x"], 45.0)
        self.assertAlmostEqual(box["corners"]["bottomLeft"]["y"], 75.0)

    def test_boxFor_behind_camera(self):
        """Box should zero out and flag behindCamera if target is behind."""
        box = arkmath.boxFor(self.camera, 0.0, 0.0, 1.0, 0.5, 10.0)
        self.assertTrue(box["behindCamera"])
        self.assertAlmostEqual(box["boxW"], 0.0)
        self.assertAlmostEqual(box["boxH"], 0.0)

if __name__ == "__main__":
    unittest.main()