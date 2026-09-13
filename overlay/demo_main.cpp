#include "arkoverlay.h"

#include <cstdio>

int main()
{
    // Option 1:
    // Default demo window.
    arkoverlay::overlayDemo(nullptr);

    // Option 2:
    // Start with a manually chosen rectangle.
    //
    // RECT r{};
    // r.left = 200;
    // r.top = 200;
    // r.right = 1480;
    // r.bottom = 1020;
    // arkoverlay::overlayDemo(&r);

    return 0;
}