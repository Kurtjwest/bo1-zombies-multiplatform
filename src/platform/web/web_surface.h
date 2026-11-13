#pragma once
// What the WebGL renderer (webgl_rhi.cpp) draws into: the page's canvas, through a canvas of the render thread's own
// whose frames it hands the page (bo1_web_lib.js). The size the game draws at and how that picture covers the canvas
// are kept current by the bridge (web_bridge.cpp); Bridge_GetMetalLayer returns this, the Direct3D 9 layer's "layer".

#include <atomic>

struct WebSurface
{
    std::atomic<int> logicalW{0}, logicalH{0}; // the size the game draws at (the back buffer), 0: the canvas's
    std::atomic<int> fill{0};                  // 1: stretched over the whole canvas, 0: at its aspect ratio, black bars
};
