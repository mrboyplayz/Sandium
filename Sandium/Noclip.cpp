#include "Noclip.hpp"

#include "Addresses.hpp"
#include "api/Text.hpp"
#include "structs/Human.hpp"

#include <cmath>

#if _WIN32
#include <Windows.h>
#undef DrawText
#endif

// Body noclip. The engine keeps a per-human physics flag (offset 0x04 in the
// Human struct, "physicsSim" in the dedicated server's struct map). Turning
// it off stops the simulation fighting us; the body is then flown by writing
// its logical position and every bone position each frame (velocities zeroed
// so nothing drifts). N toggles; physics resumes on toggle-off.

namespace noclip
{
    namespace
    {
        bool active = false;
        bool previousN = false;

        constexpr unsigned char KEY_N = 17;
        constexpr unsigned char KEY_W = 26;
        constexpr unsigned char KEY_A = 4;
        constexpr unsigned char KEY_S = 22;
        constexpr unsigned char KEY_D = 7;
        constexpr unsigned char KEY_SPACE = 44;
        constexpr unsigned char KEY_LCTRL = 224;
        constexpr unsigned char KEY_RCTRL = 228;
        constexpr unsigned char KEY_LSHIFT = 225;

        const unsigned char *Keyboard()
        {
            using SDLKeysFn = const unsigned char *(*)(const unsigned char *);
            static SDLKeysFn sdlKeys = nullptr;
            static bool resolved = false;
            if (!resolved)
            {
                resolved = true;
                const HMODULE sdl = GetModuleHandleA("SDL2.dll");
                if (sdl)
                    sdlKeys = reinterpret_cast<SDLKeysFn>(GetProcAddress(sdl, "SDL_GetKeyboardState"));
            }
            return sdlKeys ? sdlKeys(nullptr) : nullptr;
        }

        // offset 0x04 in the engine Human layout: physics simulation flag
        int &PhysicsSim(structs::Human &human)
        {
            return *reinterpret_cast<int *>(reinterpret_cast<char *>(&human) + 0x04);
        }
    }

    void Update()
    {
#if _WIN32
        const unsigned char *keys = Keyboard();
        if (!keys)
            return;

        const bool n = keys[KEY_N] != 0;
        const bool nPressed = n && !previousN;
        previousN = n;
        if (!nPressed)
        {
            if (!active)
                return;
        }
        else
        {
            active = !active;
        }

        // the local human: first active (practice tool)
        structs::Human *human = nullptr;
        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
        {
            if (addresses::Humans[h].isActive.b1)
            {
                human = &addresses::Humans[h];
                break;
            }
        }
        if (!human)
        {
            active = false;
            return;
        }

        PhysicsSim(*human) = active ? 0 : 1;
        if (!active)
            return;

        // fly: aim direction from the body's view angles
        const float yaw = human->viewYaw;
        const float pitch = human->viewPitch;
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float forward[3] = {sy * cp, sp, cy * cp};
        const float right[3] = {cy, 0.0f, -sy};

        const bool shift = keys[KEY_LSHIFT] != 0;
        const float move = (shift ? 30.0f : 10.0f) * 0.016f;
        float delta[3] = {};
        if (keys[KEY_W]) { delta[0] += forward[0] * move; delta[1] += forward[1] * move; delta[2] += forward[2] * move; }
        if (keys[KEY_S]) { delta[0] -= forward[0] * move; delta[1] -= forward[1] * move; delta[2] -= forward[2] * move; }
        if (keys[KEY_D]) { delta[0] += right[0] * move; delta[1] += right[1] * move; delta[2] += right[2] * move; }
        if (keys[KEY_A]) { delta[0] -= right[0] * move; delta[1] -= right[1] * move; delta[2] -= right[2] * move; }
        if (keys[KEY_SPACE]) delta[1] += move;
        if (keys[KEY_LCTRL] || keys[KEY_RCTRL]) delta[1] -= move;

        human->position.x += delta[0];
        human->position.y += delta[1];
        human->position.z += delta[2];
        human->alternativePosition = human->position;

        // Carry the body the way the engine's own teleport does (RosaServer
        // Human::teleport): every bone's pos AND pos2, plus the bone's rigid
        // body in the client bodies array (dword index 18215710, 47-dword
        // stride, pos at +6, vel at +9). Without the rigid bodies the
        // physics layer never moves with us.
        const auto stateBase = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + 0x404A6C;
        for (int b = 0; b < 16; ++b)
        {
            structs::Bone &bone = human->bones[b];
            bone.position.x += delta[0];
            bone.position.y += delta[1];
            bone.position.z += delta[2];
            bone.alternativePosition.x += delta[0];
            bone.alternativePosition.y += delta[1];
            bone.alternativePosition.z += delta[2];
            bone.velocity.x = 0.0f;
            bone.velocity.y = 0.0f;
            bone.velocity.z = 0.0f;

            const int rid = bone.rigidBodyID;
            if (rid >= 0 && rid < 65536)
            {
                float *body = reinterpret_cast<float *>(stateBase + 4ULL * (18215710 + 47ULL * rid));
                body[6] += delta[0];
                body[7] += delta[1];
                body[8] += delta[2];
                body[9] = 0.0f;
                body[10] = 0.0f;
                body[11] = 0.0f;
            }
        }
#endif
    }

    void Draw()
    {
#if _WIN32
        if (!active)
            return;
        api::DrawText(std::string("NOCLIP  [N] off"), 512.0f, 736.0f, 14.0f,
                      glm::vec4(1.0f, 0.6f, 0.2f, 1.0f), api::TextAlignment::Center, true);
#endif
    }
}
