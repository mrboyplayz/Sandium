#include "Freecam.hpp"

#include "../Addresses.hpp"
#include "GLUniforms.hpp"

#include <cmath>
#include <cstring>
#include <chrono>

#if _WIN32
#include <Windows.h>
#endif

namespace api
{
    namespace freecam
    {
#if _WIN32
        namespace
        {
            enum class Mode { Off, Camera, Parked };

            Mode mode = Mode::Off;
            // camera offset from the player camera while in Camera mode
            float offset[3] = {};
            // frozen camera while Parked
            float frozenMatrix[16] = {};
            bool frozenTransposed = false;
            float frozenPosition[3] = {};
            bool frozenPositionValid = false;
            bool previousF10 = false;
            std::chrono::steady_clock::time_point lastUpdate{};

            using SDLKeysFn = const unsigned char *(*)(const unsigned char *);
            SDLKeysFn sdlKeys = nullptr;
            bool sdlKeysResolved = false;

            constexpr unsigned char KEY_F10 = 67;   // SDL_SCANCODE_F10
            constexpr unsigned char KEY_LCTRL = 224;
            constexpr unsigned char KEY_RCTRL = 228;
            constexpr unsigned char KEY_Q = 20;
            constexpr unsigned char KEY_E = 8;
            constexpr unsigned char KEY_W = 26;
            constexpr unsigned char KEY_A = 4;
            constexpr unsigned char KEY_S = 22;
            constexpr unsigned char KEY_D = 7;

            const unsigned char *Keyboard()
            {
                if (!sdlKeysResolved)
                {
                    sdlKeysResolved = true;
                    const HMODULE sdl = GetModuleHandleA("SDL2.dll");
                    if (sdl)
                        sdlKeys = reinterpret_cast<SDLKeysFn>(GetProcAddress(sdl, "SDL_GetKeyboardState"));
                }
                return sdlKeys ? sdlKeys(nullptr) : nullptr;
            }

            // shift the view matrix translation so the camera sits delta further
            // along its own axes
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
#endif

        void Update()
        {
#if _WIN32
            if (!addresses::CSKeyboard.ptr)
                return;
            const unsigned char *keys = Keyboard();
            if (!keys)
                return;

            const bool f10 = keys[KEY_F10] != 0;
            const bool ctrl = keys[KEY_LCTRL] != 0 || keys[KEY_RCTRL] != 0;
            const bool f10Pressed = f10 && !previousF10;
            previousF10 = f10;

            if (f10Pressed)
            {
                if (mode == Mode::Camera && ctrl)
                {
                    // freeze the camera where it is; the player moves normally again
                    if (glcap::HasLastView())
                    {
                        float matrix[16];
                        const float *view = glcap::LastViewMatrix();
                        std::memcpy(matrix, view, sizeof(matrix));
                        frozenTransposed = glcap::LastViewTransposed();
                        ShiftView(matrix, frozenTransposed, offset);
                        std::memcpy(frozenMatrix, matrix, sizeof(frozenMatrix));
                        if (glcap::LastViewPositionValid())
                        {
                            const float *pos = glcap::LastViewPosition();
                            frozenPosition[0] = pos[0] + offset[0];
                            frozenPosition[1] = pos[1] + offset[1];
                            frozenPosition[2] = pos[2] + offset[2];
                            frozenPositionValid = true;
                        }
                        mode = Mode::Parked;
                        return;
                    }
                    mode = Mode::Off;
                    return;
                }
                if (mode == Mode::Off)
                {
                    offset[0] = offset[1] = offset[2] = 0.0f;
                    mode = Mode::Camera;
                }
                else
                {
                    mode = Mode::Off; // F10 again disables freecam
                }
            }

            if (mode != Mode::Camera)
                return;

            // fly the camera with WASD + Q/E along the player's view axes
            const auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - lastUpdate).count();
            lastUpdate = now;
            if (dt <= 0.0f || dt > 0.25f)
                dt = 0.016f;

            bool haveAxes = glcap::HasLastView();
            float right[3], forward[3];
            if (haveAxes)
            {
                const float *view = glcap::LastViewMatrix();
                if (glcap::LastViewTransposed())
                {
                    right[0] = view[0]; right[1] = view[4]; right[2] = view[8];
                    forward[0] = -view[2]; forward[1] = -view[6]; forward[2] = -view[10];
                }
                else
                {
                    right[0] = view[0]; right[1] = view[1]; right[2] = view[2];
                    forward[0] = -view[8]; forward[1] = -view[9]; forward[2] = -view[10];
                }
            }
            else
            {
                // fall back to the local player's view yaw
                for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
                {
                    if (!addresses::Humans[h].isActive.b1)
                        continue;
                    const float yaw = addresses::Humans[h].viewYaw;
                    forward[0] = std::sin(yaw); forward[1] = 0.0f; forward[2] = std::cos(yaw);
                    right[0] = std::cos(yaw); right[1] = 0.0f; right[2] = -std::sin(yaw);
                    haveAxes = true;
                    break;
                }
            }
            if (haveAxes)
            {
                const float move = 12.0f * dt;
                float delta[3] = {};
                if (keys[KEY_W]) { delta[0] += forward[0] * move; delta[1] += forward[1] * move; delta[2] += forward[2] * move; }
                if (keys[KEY_S]) { delta[0] -= forward[0] * move; delta[1] -= forward[1] * move; delta[2] -= forward[2] * move; }
                if (keys[KEY_D]) { delta[0] += right[0] * move; delta[1] += right[1] * move; delta[2] += right[2] * move; }
                if (keys[KEY_A]) { delta[0] -= right[0] * move; delta[1] -= right[1] * move; delta[2] -= right[2] * move; }
                if (keys[KEY_E]) delta[1] += move;
                if (keys[KEY_Q]) delta[1] -= move;
                offset[0] += delta[0];
                offset[1] += delta[1];
                offset[2] += delta[2];
            }

            // while flying the camera the player character stays put: strip the
            // movement keys out of the game's keyboard state
            (*addresses::CSKeyboard.ptr)[KEY_W] = false;
            (*addresses::CSKeyboard.ptr)[KEY_A] = false;
            (*addresses::CSKeyboard.ptr)[KEY_S] = false;
            (*addresses::CSKeyboard.ptr)[KEY_D] = false;
#endif
        }

        bool ViewOverride(float *matrix, bool transposed)
        {
#if _WIN32
            if (mode == Mode::Off)
                return false;
            if (mode == Mode::Parked)
            {
                std::memcpy(matrix, frozenMatrix, sizeof(frozenMatrix));
                (void)transposed; // the frozen matrix is stored in its own layout
                return true;
            }
            ShiftView(matrix, transposed, offset);
            return true;
#else
            return false;
#endif
        }

        bool ModelViewOverride(float *m)
        {
            if (mode == Mode::Off)
                return false;
            const float *delta = nullptr;
            if (mode == Mode::Camera)
                delta = offset;
            else if (frozenPositionValid)
                delta = frozenPosition;
            if (!delta)
                return false;
            // moving the camera by delta == moving the world by -delta:
            // post-multiply the composed MVP by a world-space translation
            if (!glcap::LastViewTransposed())
            {
                for (int r = 0; r < 4; ++r)
                    m[r * 4 + 3] -= m[r * 4] * delta[0] + m[r * 4 + 1] * delta[1] + m[r * 4 + 2] * delta[2];
            }
            else
            {
                for (int c = 0; c < 4; ++c)
                    m[c * 4 + 3] -= m[c * 4] * delta[0] + m[c * 4 + 1] * delta[1] + m[c * 4 + 2] * delta[2];
            }
            return true;
        }

        bool PositionOverride(float *xyz)
        {
#if _WIN32
            if (mode == Mode::Off)
                return false;
            if (mode == Mode::Camera)
            {
                xyz[0] += offset[0];
                xyz[1] += offset[1];
                xyz[2] += offset[2];
            }
            else if (mode == Mode::Parked && frozenPositionValid)
            {
                xyz[0] = frozenPosition[0];
                xyz[1] = frozenPosition[1];
                xyz[2] = frozenPosition[2];
            }
            return true;
#else
            return false;
#endif
        }
    }
}
