#include "CameraFX.hpp"

#include "Addresses.hpp"
#include "PainShader.hpp"
#include "api/GLUniforms.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>

// Pain-driven camera shake. The shake is a small translation of the view
// matrix along its own right/up axes (smooth sinusoidal wobble plus a
// deterministic pseudo-random jitter), applied to every viewmatrix upload
// through the GL uniform wrapper.

namespace camerafx
{
    namespace
    {
        constexpr bool PAIN_CAMERA_EFFECTS_ENABLED = false;
        const auto started = std::chrono::steady_clock::now();

        void ShiftView(float *m, bool transposed, const float delta[3])
        {
            float shift[3];
            if (!transposed)
            {
                shift[0] = m[0] * delta[0] + m[4] * delta[1] + m[8] * delta[2];
                shift[1] = m[1] * delta[0] + m[5] * delta[1] + m[9] * delta[2];
                shift[2] = m[2] * delta[0] + m[6] * delta[1] + m[10] * delta[2];
                m[12] -= shift[0];
                m[13] -= shift[1];
                m[14] -= shift[2];
            }
            else
            {
                shift[0] = m[0] * delta[0] + m[1] * delta[1] + m[2] * delta[2];
                shift[1] = m[4] * delta[0] + m[5] * delta[1] + m[6] * delta[2];
                shift[2] = m[8] * delta[0] + m[9] * delta[1] + m[10] * delta[2];
                m[3] -= shift[0];
                m[7] -= shift[1];
                m[11] -= shift[2];
            }
        }
    }

    void Update()
    {
    }

    bool ViewOverride(float *matrix, bool transposed)
    {
#if _WIN32
        if (!PAIN_CAMERA_EFFECTS_ENABLED)
            return false;
        const float pain01 = painshader::Intensity();
        const float amplitude = std::clamp((pain01 - 0.6f) / 0.4f, 0.0f, 1.0f) * 0.045f;
        if (amplitude <= 0.0f || !api::glcap::HasLastView())
            return false;

        const float t = std::chrono::duration<float>(
                            std::chrono::steady_clock::now() - started).count();
        // smooth wobble + irregular jitter
        const float dx = std::sin(t * 13.0f) * 0.6f + std::sin(t * 29.7f + 1.3f) * 0.4f;
        const float dy = std::sin(t * 17.3f + 0.6f) * 0.6f + std::sin(t * 23.1f) * 0.4f;

        const float *view = api::glcap::LastViewMatrix();
        float right[3], up[3];
        if (api::glcap::LastViewTransposed())
        {
            right[0] = view[0]; right[1] = view[4]; right[2] = view[8];
            up[0] = view[1]; up[1] = view[5]; up[2] = view[9];
        }
        else
        {
            right[0] = view[0]; right[1] = view[1]; right[2] = view[2];
            up[0] = view[4]; up[1] = view[5]; up[2] = view[6];
        }
        const float delta[3] = {
            right[0] * dx * amplitude + up[0] * dy * amplitude,
            right[1] * dx * amplitude + up[1] * dy * amplitude,
            right[2] * dx * amplitude + up[2] * dy * amplitude,
        };
        ShiftView(matrix, transposed, delta);
        return true;
#else
        return false;
#endif
    }

    float BlurAmount()
    {
#if _WIN32
        if (!PAIN_CAMERA_EFFECTS_ENABLED)
            return 0.0f;
        const float pain01 = painshader::Intensity();
        return std::clamp((pain01 - 0.7f) / 0.3f, 0.0f, 1.0f);
#else
        return 0.0f;
#endif
    }
}
