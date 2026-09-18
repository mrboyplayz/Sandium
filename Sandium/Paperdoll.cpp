#include "Paperdoll.hpp"
#include "PaperdollGeometry.hpp"
#include "Addresses.hpp"
#include "api/Text.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

#include "structs/Human.hpp"

#if _WIN32
#include <Windows.h>
#include <intrin.h>
#include <subhook.h>
#undef DrawText
#endif

namespace paperdoll
{
    namespace
    {
        int selectedShape = static_cast<int>(Shape::Squares);
        bool loaded = false;
        bool available = false;
        bool saveFailed = false;

        // ---- throw pose (hold Q with an item in hand) ----
        constexpr int THROW_ARM_START = 7;   // right shoulder
        constexpr int THROW_ARM_END = 9;     // right hand
        constexpr int THROW_HAND_BONE = 9;
        constexpr float THROW_TURN = 1.04719755f; // 60 degrees: turn the doll to look left
        float throwBlend = 0.0f;             // 0 = normal, 1 = full throw pose
        int paperdollFlushIndex = 0;
        bool sdlKeysResolved = false;
        using SDLKeysFn = const unsigned char *(*)(const unsigned char *);
        SDLKeysFn sdlKeys = nullptr;

        bool QKeyHeld()
        {
            if (!sdlKeysResolved)
            {
                sdlKeysResolved = true;
                const HMODULE sdl = GetModuleHandleA("SDL2.dll");
                if (sdl)
                    sdlKeys = reinterpret_cast<SDLKeysFn>(GetProcAddress(sdl, "SDL_GetKeyboardState"));
            }
            constexpr unsigned char SDL_SCANCODE_Q = 20;
            return sdlKeys && sdlKeys(nullptr)[SDL_SCANCODE_Q];
        }

        bool HumanHasItemInRightHand(int human)
        {
            return human >= 0 && human < static_cast<int>(structs::Human::VanillaCount) &&
                   addresses::Humans[human].isActive.b1 &&
                   addresses::Humans[human].inventorySlots[0].numberOfPlaces > 0;
        }
        constexpr const char* configPath = "sandium/paperdoll.txt";
        constexpr const char* names[] = {"circles", "squares", "triangles", "octagons"};
        constexpr const char* labels[] = {"Circles (vanilla)", "Squares", "Triangles", "Octagons"};

        void Load()
        {
            if (loaded) return;
            loaded = true;
            std::ifstream file(configPath);
            std::string name;
            if (file >> name)
                for (int i = 0; i < 4; ++i)
                    if (name == names[i]) selectedShape = i;
        }

        void Save()
        {
            std::ofstream file(configPath, std::ios::trunc);
            file << names[selectedShape] << '\n';
            file.close();
            saveFailed = !file;
        }

#if _WIN32
        // Sub Rosa 38f client, SHA256 e9500422582b7e550be25f6b31e8cdf00b728ae733d5444868c89982c1526c5e.
        // Verified against the local executable and its recovered HUD renderer.
        constexpr std::uintptr_t drawPaperdollRva = 0xA7C70;
        constexpr std::uintptr_t drawFlushRva = 0x6E0D0;
        constexpr std::uintptr_t primitiveRva = 0x6D059440;
        constexpr std::uintptr_t countRva = 0x6D059448;
        constexpr std::uintptr_t strideRva = 0x6D05944C;
        constexpr std::uintptr_t verticesRva = 0x6D059450;
        using DrawPaperdoll = void (*)(int, float, float);
        using DrawFlush = void (*)();
        DrawPaperdoll originalPaperdoll = nullptr;
        DrawFlush originalFlush = nullptr;
        subhook::Hook paperdollHook, flushHook;
        thread_local bool drawingPaperdoll = false;

        template<typename T> T& At(std::uintptr_t rva)
        {
            return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + rva);
        }

        void FlushHook()
        {
            subhook::ScopedHookRemove remove(&flushHook);
            const int primitive = At<int>(primitiveRva);
            int& count = At<int>(countRva);
            // The injury fill can arrive through more than one internal return
            // path. Its buffered layout is stable and unambiguous while inside
            // DrawPaperdoll: triangle fan capsules have 192 vertices and outline
            // capsules have 128 line vertices.
            const bool fill = primitive == 0 && count == 192;
            const bool outline = primitive == 2 && count == 128;
            const bool capsule = drawingPaperdoll && At<int>(strideRva) == 9 &&
                (outline || fill);
            if (drawingPaperdoll && capsule)
            {
                // limbs draw in bone order (0..15); map this flush to a bone
                const int bone = paperdollFlushIndex++;
                const bool throwing = throwBlend > 0.01f;
                if (throwing)
                {
                    float* buffer = &At<float>(verticesRva);
                    // highlight the throwing arm green and the hand yellow
                    // (vertex layout: x y z u v r g b a)
                    if (bone >= THROW_ARM_START && bone <= THROW_ARM_END)
                    {
                        const bool hand = bone == THROW_HAND_BONE;
                        for (int v = 0; v < count; ++v)
                        {
                            buffer[v * 9 + 5] = hand ? 1.0f : 0.15f;
                            buffer[v * 9 + 6] = hand ? 0.9f : 1.0f;
                            buffer[v * 9 + 7] = hand ? 0.15f : 0.25f;
                            buffer[v * 9 + 8] = 1.0f;
                        }
                    }
                }
            }
            if (drawingPaperdoll && selectedShape != 0 && capsule)
            {
                float* buffer = &At<float>(verticesRva);
                const auto sample = [=](int i) {
                    const int vertex = outline ? i * 2 : i * 3 + 1;
                    return Point{buffer[vertex * 9], buffer[vertex * 9 + 1]};
                };
                const auto polygon = MakePolygon(static_cast<Shape>(selectedShape), sample(0), sample(16), sample(48));
                if (polygon.count)
                {
                    // Preserve z, texture coordinates and the game's per-part injury RGBA.
                    std::array<float, 9> attributes;
                    std::memcpy(attributes.data(), buffer, sizeof(attributes));
                    int written = 0;
                    const auto vertex = [&](Point point) {
                        float* out = buffer + written++ * 9;
                        std::memcpy(out, attributes.data(), sizeof(attributes));
                        out[0] = point.x;
                        out[1] = point.y;
                    };
                    for (int i = 0; i < polygon.count; ++i)
                    {
                        if (fill) vertex(polygon.center);
                        vertex(polygon.points[i]);
                        vertex(polygon.points[(i + 1) % polygon.count]);
                    }
                    count = written;
                }
            }
            originalFlush();
        }

        void PaperdollHook(int human, float x, float y)
        {
            subhook::ScopedHookRemove remove(&paperdollHook);
            Load();

            // ease the throw pose in while Q is held with an item in the right
            // hand, and ease back out once the throw happens (Q released)
            const bool throwing = QKeyHeld() && HumanHasItemInRightHand(human);
            throwBlend += ((throwing ? 1.0f : 0.0f) - throwBlend) * 0.18f;
            paperdollFlushIndex = 0;

            // DrawPaperdoll builds its 3D matrix from Human::viewYaw. Offset that
            // source value only during this call, so the actual paperdoll camera
            // orbits left while the player's real view and aiming stay unchanged.
            float &viewYaw = addresses::Humans[human].viewYaw;
            const float savedViewYaw = viewYaw;
            viewYaw += THROW_TURN * throwBlend;

            if (selectedShape == 0)
            {
                originalPaperdoll(human, x, y);
                viewYaw = savedViewYaw;
                return;
            }
            // Only intercept flushes during this HUD element, never the world or other UI.
            subhook::ScopedHookInstall install(&flushHook);
            drawingPaperdoll = true;
            originalPaperdoll(human, x, y);
            drawingPaperdoll = false;
            viewYaw = savedViewYaw;
        }
#endif
    }

    void Install()
    {
#if _WIN32
        if (*addresses::IsDedicated) return;
        const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
        constexpr unsigned char prologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18};
        constexpr unsigned char flushPrologue[] = {0x48, 0x83, 0xEC, 0x38, 0x83, 0x3D};
        if (std::memcmp(reinterpret_cast<void*>(base + drawPaperdollRva), prologue, sizeof(prologue)) ||
            std::memcmp(reinterpret_cast<void*>(base + drawFlushRva), flushPrologue, sizeof(flushPrologue))) return;
        originalPaperdoll = reinterpret_cast<DrawPaperdoll>(base + drawPaperdollRva);
        originalFlush = reinterpret_cast<DrawFlush>(base + drawFlushRva);
        if (!flushHook.Install(reinterpret_cast<void*>(originalFlush), reinterpret_cast<void*>(&FlushHook), subhook::HookFlag64BitOffset)) return;
        flushHook.Remove();
        available = paperdollHook.Install(reinterpret_cast<void*>(originalPaperdoll), reinterpret_cast<void*>(&PaperdollHook), subhook::HookFlag64BitOffset);
#endif
    }

    void DrawSettings()
    {
        Load();
        api::DrawText("paperdoll shape", 8.0f, 92.0f, 22.0f, glm::vec4(1.0f), api::TextAlignment::Right);
        if (!available)
        {
            api::DrawText("Unavailable for this game build", 8.0f, 124.0f, 18.0f, glm::vec4(1.0f), api::TextAlignment::Right);
            return;
        }
        const int before = selectedShape;
        for (int i = 0; i < 4; ++i)
        {
            *addresses::NextMenuButtonPositionX = 8.0f;
            *addresses::NextMenuButtonPositionY = 128.0f + i * 38.0f;
            *addresses::NextMenuButtonSizeX = 240.0f;
            *addresses::NextMenuButtonSizeY = 32.0f;
            *addresses::NextMenuButtonKey = static_cast<SDL_Scancode>(-1);
            addresses::DrawMenuButtonSelectableFunc(labels[i], &selectedShape, i);
        }
        if (selectedShape != before) Save();
        if (saveFailed)
            api::DrawText("Could not save paperdoll.txt", 8.0f, 350.0f, 18.0f, glm::vec4(1.0f, 0.4f, 0.4f, 1.0f), api::TextAlignment::Right);
    }
}
