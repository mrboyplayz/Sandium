#pragma once

namespace api
{
    namespace freecam
    {
        // Per-frame key handling and movement. Call once per rendered frame.
        void Update();

        // Called by the GL uniform wrappers for every "viewmatrix" upload.
        // Returns true and overwrites the matrix when the freecam is active.
        bool ViewOverride(float *matrix16, bool transposed);

        // Same for "viewposition" uploads (camera world position).
        bool PositionOverride(float *xyz);

        // Post-multiplies the camera delta onto composed MVP uploads so the
        // whole scene renders from the freecam position.
        bool ModelViewOverride(float *matrix16);
    }
}
