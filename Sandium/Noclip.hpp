#pragma once

namespace noclip
{
    // Body noclip: toggle with N. Disables the local human's physics and
    // flies the actual body (position + all bones) with WASD/Space/Ctrl,
    // aiming with the mouse. Windows only.
    void Update();
    void Draw();
}
