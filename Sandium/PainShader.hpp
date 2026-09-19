#pragma once

namespace painshader
{
    // Pain feedback overlay: a red vignette that grows in as the local human
    // loses health ("pain progressing"), flashes harder on each hit and
    // slowly pulses while heavily wounded. Update() once per frame; Draw()
    // inside the HUD's Lua drawing window. Windows only.
    void Update();
    void Draw();
}
