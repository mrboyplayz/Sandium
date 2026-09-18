#include "NativeFreecam.hpp"

#include "Addresses.hpp"
#include "api/Logging.hpp"

#if _WIN32
#include <Windows.h>
#endif

#include <cstdint>
#include <cstring>

namespace nativefreecam
{
#if _WIN32
    namespace
    {
        using SDLGetKeyboardState = const unsigned char *(*)(int *);
        SDLGetKeyboardState getKeyboardState = nullptr;
        bool previousSix = false;
        bool previousSeven = false;
        constexpr std::size_t cameraSelectionJumpRva = 0x97567;
        constexpr unsigned char cameraSelectionJump[] = {0x0F, 0x84, 0xE9, 0x00, 0x00, 0x00};

        template<typename T> T &At(std::size_t rva)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            return *reinterpret_cast<T *>(base + rva);
        }

        std::uint32_t &GameDword(std::size_t index)
        {
            return At<std::uint32_t>(0x404A6C + index * 4);
        }

        unsigned char *GameKeys()
        {
            // dword_140404A6C[284024765], the current 512-byte SDL state.
            return reinterpret_cast<unsigned char *>(&GameDword(284024765));
        }

        void SelectDebugCameraPath(bool enabled)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            auto *instruction = reinterpret_cast<unsigned char *>(base + cameraSelectionJumpRva);
            DWORD oldProtection = 0;
            if (!VirtualProtect(instruction, sizeof(cameraSelectionJump), PAGE_EXECUTE_READWRITE, &oldProtection))
                return;
            if (enabled)
                std::memset(instruction, 0x90, sizeof(cameraSelectionJump));
            else
                std::memcpy(instruction, cameraSelectionJump, sizeof(cameraSelectionJump));
            FlushInstructionCache(GetCurrentProcess(), instruction, sizeof(cameraSelectionJump));
            VirtualProtect(instruction, sizeof(cameraSelectionJump), oldProtection, &oldProtection);
        }

        void Enter()
        {
            // Camera 2 occupies seven floats starting at index 284024395.
            // Seed it from the renderer's current camera position and angles.
            for (std::size_t i = 0; i < 6; ++i)
                GameDword(284024395 + i) = GameDword(284024388 + i);
            GameDword(19065878) = 2;
            GameDword(284021982) = static_cast<std::uint32_t>(-1);
            SelectDebugCameraPath(true);
            api::GetSandiumLogger()->LogText("Native freecam enabled (6)");
        }

        void Exit()
        {
            GameDword(19065878) = 3;
            SelectDebugCameraPath(false);
            api::GetSandiumLogger()->LogText("Native freecam disabled (7)");
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
        if (!getKeyboardState || !*addresses::IsInGame) return;

        int keyCount = 0;
        const unsigned char *keys = getKeyboardState(&keyCount);
        if (!keys || keyCount <= 36) return;
        const bool six = keys[35] != 0;
        const bool seven = keys[36] != 0;
        if (six && !previousSix) Enter();
        if (seven && !previousSeven) Exit();
        previousSix = six;
        previousSeven = seven;

        if (GameDword(19065878) != 2) return;

        // Feed live movement state to the game's native camera routine. The
        // retail frame path copies/clears this state before the HUD is drawn.
        unsigned char *gameKeys = GameKeys();
        constexpr int movementKeys[] = {4, 7, 20, 22, 26, 29, 225, 229}; // A,D,Q,S,W,Z, shifts
        for (int scancode : movementKeys)
            gameKeys[scancode] = scancode < keyCount ? keys[scancode] : 0;

        using FreecamUpdate = std::int64_t (*)(std::int64_t);
        const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
        reinterpret_cast<FreecamUpdate>(base + 0x105470)(2);
#endif
    }
}
