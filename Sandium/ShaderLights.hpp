#pragma once

// Real point lights: hooks the game's GL resolver (RVA 0xE42E0) so that when
// it stores the glShaderSource pointer (state slot RVA 0x4042D8) we substitute
// our own. Every world fragment shader then gets point-light uniforms and a
// lambert-falloff block injected into its GLSL on the way to the driver.
// Lighting.cpp uploads the uniform arrays every frame.
namespace shaderlights
{
    // Call once at DLL attach, after addresses::Map (needs the exe base).
    void Install();
}
