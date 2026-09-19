#pragma once

namespace camerafx
{
    // Pain-driven camera effects. Update() once per frame; ViewOverride is
    // called from the GL uniform wrapper for every viewmatrix upload.
    // Shake amplitude rises from pain 6 to 10. BlurAmount() feeds the post
    // pass (0..1). Windows only.
    void Update();
    bool ViewOverride(float *matrix16, bool transposed);
    float BlurAmount();
}
