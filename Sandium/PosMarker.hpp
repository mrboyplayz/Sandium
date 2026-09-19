#pragma once

namespace posmarker
{
    // In-world position marking for map work. While the freecam is engaged:
    //   Z = create a marker at the camera (position + forward axis)
    //   C = clear the session markers
    // Markers render as numbered labels projected into the world and append
    // to sandium/positions.txt ("pos x y z fx fy fz" lines).
    // Call Update() once per frame (any time) and Draw() inside the HUD's
    // Lua drawing window. Windows only.
    void Update();
    void Draw();
}
