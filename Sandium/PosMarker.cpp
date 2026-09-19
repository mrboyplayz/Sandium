#include "PosMarker.hpp"

#include "Addresses.hpp"
#include "api/Freecam.hpp"
#include "api/GLUniforms.hpp"
#include "api/Text.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#if _WIN32
#include <Windows.h>
#undef DrawText
#endif

namespace posmarker
{
    namespace
    {
        struct Marker
        {
            float pos[3];
            float fwd[3];
        };

        std::vector<Marker> markers;
        bool previousZ = false;
        bool previousC = false;

        constexpr unsigned char KEY_Z = 29;
        constexpr unsigned char KEY_C = 6;

        // Sub Rosa's image/video layer UI space (matches the Lua Image API).
        constexpr float UI_WIDTH = 1024.0f;
        constexpr float UI_HEIGHT = 768.0f;

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

        bool ProjectPoint(const float *m, bool transposed, const float point[3], float ndcOut[2])
        {
            float clip[4];
            for (int row = 0; row < 4; ++row)
            {
                clip[row] = transposed
                    ? m[row * 4 + 0] * point[0] + m[row * 4 + 1] * point[1] + m[row * 4 + 2] * point[2] + m[row * 4 + 3]
                    : m[0 * 4 + row] * point[0] + m[1 * 4 + row] * point[1] + m[2 * 4 + row] * point[2] + m[3 * 4 + row];
            }
            if (clip[3] < 0.05f)
                return false;
            ndcOut[0] = clip[0] / clip[3];
            ndcOut[1] = clip[1] / clip[3];
            return true;
        }

        void CreateMarker()
        {
            Marker marker;
            if (!api::freecam::GetCamera(marker.pos, marker.fwd))
                return;
            markers.push_back(marker);
            if (std::FILE *f = std::fopen("sandium/positions.txt", "a"))
            {
                std::fprintf(f, "pos %.3f %.3f %.3f %.4f %.4f %.4f\n",
                             marker.pos[0], marker.pos[1], marker.pos[2],
                             marker.fwd[0], marker.fwd[1], marker.fwd[2]);
                std::fclose(f);
            }
        }
    }

    void Update()
    {
#if _WIN32
        if (!api::freecam::IsActive())
        {
            previousZ = previousC = false;
            return;
        }
        const unsigned char *keys = Keyboard();
        if (!keys)
            return;

        const bool z = keys[KEY_Z] != 0;
        const bool c = keys[KEY_C] != 0;
        if (z && !previousZ)
            CreateMarker();
        if (c && !previousC)
            markers.clear();
        previousZ = z;
        previousC = c;
#endif
    }

    void Draw()
    {
#if _WIN32
        if (markers.empty())
            return;
        if (!api::freecam::IsActive())
            return;
        if (!api::glcap::HasViewProjection())
            return;
        const float *matrix = api::glcap::ViewProjectionMatrix();
        const bool transposed = api::glcap::MatrixTransposed();

        int index = 1;
        for (const Marker &marker : markers)
        {
            const float label[3] = {marker.pos[0], marker.pos[1] + 0.4f, marker.pos[2]};
            float ndc[2];
            if (ProjectPoint(matrix, transposed, label, ndc))
            {
                const float x = (ndc[0] * 0.5f + 0.5f) * UI_WIDTH;
                const float y = (1.0f - (ndc[1] * 0.5f + 0.5f)) * UI_HEIGHT;
                char text[160];
                std::snprintf(text, sizeof(text), "#%d  %.2f %.2f %.2f", index,
                              marker.pos[0], marker.pos[1], marker.pos[2]);
                api::DrawText(std::string(text), x, y, 13.0f,
                              glm::vec4(0.3f, 1.0f, 0.4f, 1.0f), api::TextAlignment::Center, true);
            }
            ++index;
        }

        char hud[128];
        std::snprintf(hud, sizeof(hud), "%d markers  [Z] create  [C] clear  -> sandium/positions.txt",
                      (int)markers.size());
        api::DrawText(std::string(hud), 512.0f, 12.0f, 12.0f,
                      glm::vec4(0.3f, 1.0f, 0.4f, 1.0f), api::TextAlignment::Center, true);
#endif
    }
}
