#pragma once

namespace footsteps
{
    // Called once per rendered frame. Tracks grounded human travel and emits
    // native HRTF one-shots at each character's feet.
    void Update();
}
