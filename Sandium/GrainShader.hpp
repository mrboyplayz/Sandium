#pragma once

namespace grainshader
{
    // Reimplementation of Source's zb_grain/zb_grainwhite pixel shaders
    // (decompiled from the VCS bytecode): animated procedural white noise
    // with 1/distance² edge weighting, drawn additively over the finished
    // frame. Intensity follows the pain level (painshader::Intensity()).
    // Call Draw() once per frame, after the HUD is on screen. Windows only.
    void Draw();
}
