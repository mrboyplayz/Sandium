#include "NativeFreecam.hpp"

#include "Addresses.hpp"
#include "api/Logging.hpp"

#if _WIN32
#include <Windows.h>
#endif

#include <cstdint>
#include <cmath>
#include <cctype>
#include <cstring>

namespace nativefreecam
{
#if _WIN32
    namespace
    {
        using SDLGetKeyboardState = const unsigned char *(*)(int *);
        SDLGetKeyboardState getKeyboardState = nullptr;
        bool previousF5 = false;
        int cameraMode = 0; // 0 first person, 1 behind, 2 front-facing
        constexpr std::size_t cameraOverrideIndex = 19065870;
        constexpr std::size_t cameraSelectionIndex = 19065878;
        constexpr std::size_t cameraTargetIndex = 284021982;
        constexpr std::size_t activeCameraIndex = 284024388;
        constexpr std::size_t thirdPersonCameraIndex = 284024395;
        int previousCameraOverride = 0;
        int previousCameraSelection = 0;
        int previousCameraTarget = -1;

        template<typename T> T &At(std::size_t rva)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            return *reinterpret_cast<T *>(base + rva);
        }

        std::uint32_t &GameDword(std::size_t index)
        {
            return At<std::uint32_t>(0x404A6C + index * 4);
        }

        float &GameFloat(std::size_t index)
        {
            return reinterpret_cast<float &>(GameDword(index));
        }

        unsigned char *GameKeys()
        {
            // dword_140404A6C[284024765], the current 512-byte SDL state.
            return reinterpret_cast<unsigned char *>(&GameDword(284024765));
        }

        void Enter(int mode)
        {
            previousCameraOverride = static_cast<int>(GameDword(cameraOverrideIndex));
            previousCameraSelection = static_cast<int>(GameDword(cameraSelectionIndex));
            previousCameraTarget = static_cast<int>(GameDword(cameraTargetIndex));

            // Camera 2 occupies seven floats starting at index 284024395.
            // Seed it from the renderer's current camera position and angles.
            for (std::size_t i = 0; i < 6; ++i)
                GameDword(thirdPersonCameraIndex + i) = GameDword(activeCameraIndex + i);

            // The renderer only consults the selected camera slot while the
            // camera-override flag is enabled. Keep all related state together
            // instead of patching a conditional jump in the game code.
            GameDword(cameraOverrideIndex) = 1;
            GameDword(cameraSelectionIndex) = 2;
            GameDword(cameraTargetIndex) = static_cast<std::uint32_t>(-1);
            api::GetSandiumLogger()->LogText(
                mode == 1 ? "Third-person camera: behind player" : "Third-person camera: facing player");
        }

        void Exit()
        {
            GameDword(cameraOverrideIndex) = static_cast<std::uint32_t>(previousCameraOverride);
            GameDword(cameraSelectionIndex) = static_cast<std::uint32_t>(previousCameraSelection);
            GameDword(cameraTargetIndex) = static_cast<std::uint32_t>(previousCameraTarget);
            api::GetSandiumLogger()->LogText("Third-person camera: first person");
        }

        void CycleCameraMode()
        {
            const int oldMode = cameraMode;
            cameraMode = (cameraMode + 1) % 3;
            if (!oldMode && cameraMode)
                Enter(cameraMode);
            else if (oldMode && !cameraMode)
                Exit();
            else if (cameraMode == 1)
                api::GetSandiumLogger()->LogText("Third-person camera: behind player");
            else if (cameraMode == 2)
                api::GetSandiumLogger()->LogText("Third-person camera: facing player");
        }

        void CheckChatCommand()
        {
            if (!addresses::Base.ptr)
                return;

            static char previousText[61] = {};
            auto *chat = reinterpret_cast<char *>(
                reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) +
                0x404A6C + 4ULL * 283785913);
            char text[61] = {};
            std::size_t length = 0;
            for (; length < 60 && chat[length]; ++length)
                text[length] = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(chat[length])));

            if (std::strcmp(text, previousText) == 0)
                return;
            std::strncpy(previousText, text, sizeof(previousText) - 1);

            if (std::strcmp(text, "thirdperson") != 0 &&
                std::strcmp(text, "/thirdperson") != 0)
                return;

            CycleCameraMode();
            // Consume the local-only command so it isn't sent to the server as
            // ordinary chat text.
            std::memset(chat, 0, 60);
            previousText[0] = 0;
        }
    }
#endif

    void Update()
    {
#if _WIN32
        if (!getKeyboardState)
        {
            const HMODULE sdl = GetModuleHandleA("SDL2.dll");
            if (sdl)
                getKeyboardState = reinterpret_cast<SDLGetKeyboardState>(GetProcAddress(sdl, "SDL_GetKeyboardState"));
        }
        if (!getKeyboardState || !*addresses::IsInGame)
        {
            previousF5 = false;
            if (cameraMode) { cameraMode = 0; Exit(); }
            return;
        }

        int keyCount = 0;
        const unsigned char *keys = getKeyboardState(&keyCount);
        CheckChatCommand();
        constexpr int PageUpScancode = 75;
        if (!keys || keyCount <= PageUpScancode) return;
        const bool cyclePressed = keys[PageUpScancode] != 0;
        if (cyclePressed && !previousF5)
            CycleCameraMode();
        previousF5 = cyclePressed;
        if (!cameraMode) return;

        // The game updates its camera selection during the frame; reassert the
        // override here so it is present for the next world-render pass too.
        GameDword(cameraOverrideIndex) = 1;
        GameDword(cameraSelectionIndex) = 2;
        GameDword(cameraTargetIndex) = static_cast<std::uint32_t>(-1);

        const structs::Human *human = nullptr;
        for (std::size_t index = 0; index < structs::Human::VanillaCount; ++index)
            if (addresses::Humans[index].isActive.b1) { human = &addresses::Humans[index]; break; }
        if (!human) return;

        constexpr float Pi = 3.14159265358979323846f;
        const float yaw = human->viewYaw;
        const float forwardX = std::sin(yaw), forwardZ = std::cos(yaw);
        const float side = cameraMode == 1 ? -1.0f : 1.0f;
        const auto &head = human->bones[3].position;
        GameFloat(thirdPersonCameraIndex) = head.x + forwardX * 3.0f * side;
        GameFloat(thirdPersonCameraIndex + 1) = head.y + 0.35f;
        GameFloat(thirdPersonCameraIndex + 2) = head.z + forwardZ * 3.0f * side;
        GameFloat(thirdPersonCameraIndex + 3) = cameraMode == 1 ? yaw : yaw + Pi;
        GameFloat(thirdPersonCameraIndex + 4) = 0.10f;
        GameFloat(thirdPersonCameraIndex + 5) = 0.0f;
#endif
    }
}
