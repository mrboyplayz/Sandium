#pragma once

namespace painshader
{
    // Pain feedback overlay: a red vignette that grows in as the local human
    // loses health ("pain progressing"), flashes harder on each hit and
    // slowly pulses while heavily wounded. Update() once per frame; Draw()
    // inside the HUD's Lua drawing window. Windows only.
    void Update();
    void Draw();

    // Current pain level, 0 (healthy) .. ~1 (near death / just slammed).
    float Intensity();

    // Current pain value on the 0-10 scale.
    float PainLevel();

    // 1 while knocked out.
    int Uncon();

    // Black-screen fade for the knockout (0..1).
    float BlackFade();

    // Framebuffer blur amount from severe pain (0..1).
    float Blur();
}
