#pragma once

#include <cstdint>

namespace api
{
    namespace glcap
    {
        // Captures Sub Rosa's camera matrices by intercepting the GL uniform
        // calls the renderer uses to feed its shaders. Install once per process
        // after the GL context exists.
        void Install();

        // Call with the GL context current on the main thread; resolves the real
        // entry points and starts the background pointer scan.
        void Prepare();

        // Sandium's configurable gameplay camera FOV. The value is persisted
        // separately because stock Sub Rosa does not expose an FOV setting.
        int FieldOfView();
        void SetFieldOfView(int degrees);

        // Writes per-program capture state (upload counts, camera positions)
        // to the shared sandium_log.txt for diagnosing wrong-pass captures.
        void DumpDiagnostics();

        // Last uploaded camera view matrix / position (any program).
        bool HasLastView();
        const float *LastViewMatrix();
        bool LastViewTransposed();
        const float *LastViewPosition();
        bool LastViewPositionValid();

        bool HasViewProjection();
        const float *ViewProjectionMatrix(); // 16 floats, as uploaded
        bool MatrixTransposed();             // the transpose flag of the last upload
        bool HasViewPosition();
        const float *ViewPosition();         // xyz of the last viewposition upload
    }
}
