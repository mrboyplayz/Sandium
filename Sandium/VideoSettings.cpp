#include "VideoSettings.hpp"

#include "Addresses.hpp"
#include "Flags.hpp"
#include "LuaManager.hpp"
#include "api/Image.hpp"
#include "api/Text.hpp"
#include <subhook.h>
#if _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>

namespace videosettings
{
#if _WIN32
    namespace
    {
        using SwapWindow = void (__cdecl *)(void *);
        using ShowCursor = int (__cdecl *)(int);
        using SetRelativeMouseMode = int (__cdecl *)(int);
        using SetFullscreen = int (__cdecl *)(void *, unsigned int);
        using SetBordered = void (__cdecl *)(void *, int);
        using SetWindowSize = void (__cdecl *)(void *, int, int);
        using SetWindowPosition = void (__cdecl *)(void *, int, int);
        using GetWindowSize = void (__cdecl *)(void *, int *, int *);
        using GetWindowPosition = void (__cdecl *)(void *, int *, int *);
        using GetWindowFlags = unsigned int (__cdecl *)(void *);
        using GetDisplayIndex = int (__cdecl *)(void *);
        using GetDisplayBounds = int (__cdecl *)(int, void *);
        using GetDesktopDisplayMode = int (__cdecl *)(int, void *);
        using SetSwapInterval = int (__cdecl *)(int);
        using GetSwapInterval = int (__cdecl *)();

        struct Rect { int x, y, w, h; };
        struct DisplayMode { unsigned int format; int w, h, refreshRate; void *driverData; };

        HMODULE sdl = nullptr;
        SwapWindow originalSwap = nullptr;
        ShowCursor showCursor = nullptr;
        SetRelativeMouseMode setRelativeMouseMode = nullptr;
        SetFullscreen setFullscreen = nullptr;
        SetFullscreen originalSetFullscreen = nullptr;
        SetBordered setBordered = nullptr;
        SetWindowSize setSize = nullptr;
        SetWindowPosition setPosition = nullptr;
        GetWindowSize getSize = nullptr;
        GetWindowPosition getPosition = nullptr;
        GetWindowFlags getFlags = nullptr;
        GetDisplayIndex getDisplayIndex = nullptr;
        GetDisplayBounds getDisplayBounds = nullptr;
        GetDesktopDisplayMode getDesktopMode = nullptr;
        SetSwapInterval setSwapInterval = nullptr;
        GetSwapInterval getSwapInterval = nullptr;
        subhook::Hook *swapHook = nullptr;
        subhook::Hook *fullscreenHook = nullptr;

        bool initialized = false, configLoaded = false, borderless = false;
        bool unlockFps = false, savedWindowState = false, originalFullscreen = false;
        bool swapIntervalOverridden = false;
        int maximumFps = 144, originalSwapInterval = 1;
        int originalX = 0, originalY = 0, originalWidth = 0, originalHeight = 0;
        std::chrono::steady_clock::time_point nextFrame{};
        HANDLE frameTimer = nullptr;
        constexpr std::uintptr_t maximizeFullscreenBranchRva = 0x7CDA5;

        void WaitUntil(std::chrono::steady_clock::time_point deadline)
        {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) return;
            if (frameTimer && remaining > std::chrono::milliseconds(2))
            {
                const auto waitTicks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    remaining - std::chrono::milliseconds(1)).count() / 100;
                LARGE_INTEGER due{};
                due.QuadPart = -std::max<LONGLONG>(1, waitTicks);
                if (SetWaitableTimerEx(frameTimer, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject(frameTimer, INFINITE);
            }
            std::this_thread::sleep_until(deadline);
        }

        void LoadConfig()
        {
            if (configLoaded) return;
            configLoaded = true;
            std::ifstream file("sandium_video.txt");
            int borderlessValue = 0, unlockValue = 0, fpsValue = 144;
            if (file >> borderlessValue >> unlockValue >> fpsValue)
            {
                borderless = borderlessValue != 0;
                unlockFps = unlockValue != 0;
                maximumFps = std::clamp(fpsValue, 30, 360);
            }
        }

        void SaveConfig()
        {
            std::ofstream file("sandium_video.txt", std::ios::trunc);
            if (file) file << (borderless ? 1 : 0) << ' ' << (unlockFps ? 1 : 0)
                          << ' ' << maximumFps << '\n';
        }

        void SetMaximizeFullscreenBypass(bool enabled)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            if (!base) return;
            auto *branch = reinterpret_cast<std::uint8_t *>(base + maximizeFullscreenBranchRva);
            const std::uint8_t compareBytes[] = { 0x80, 0x7C, 0x24, 0x34, 0x08 };
            if (std::memcmp(branch - 5, compareBytes, sizeof(compareBytes)) != 0) return;
            DWORD oldProtect = 0;
            if (!VirtualProtect(branch, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) return;

            // IDA: sub_14007CC30 handles SDL_WINDOWEVENT_MAXIMIZED at RVA
            // 0x7CDA0 and its JNZ at 0x7CDA5 skips the explicit fullscreen
            // call. Force that branch only while borderless mode is active.
            if (branch[1] == 0x30)
            {
                if (enabled && branch[0] == 0x75) branch[0] = 0xEB;
                else if (!enabled && branch[0] == 0xEB) branch[0] = 0x75;
            }
            FlushInstructionCache(GetCurrentProcess(), branch, 2);
            DWORD ignored = 0;
            VirtualProtect(branch, 2, oldProtect, &ignored);
        }

        void ApplyBorderless(bool enable)
        {
            void *window = addresses::SDLWindowPtr.ptr ? *addresses::SDLWindowPtr : nullptr;
            if (!window || !setFullscreen || !setBordered || !setSize || !setPosition ||
                !getSize || !getPosition || !getFlags) return;

            borderless = enable;
            SetMaximizeFullscreenBypass(enable);

            if (enable)
            {
                getSize(window, &originalWidth, &originalHeight);
                getPosition(window, &originalX, &originalY);
                originalFullscreen = (getFlags(window) & 0x1u) != 0;
                savedWindowState = true;
                // Leave SDL fullscreen mode, then size a normal borderless
                // window to the full display. Do not set SDL's fullscreen flag.
                setFullscreen(window, 0);
                setBordered(window, 0);
                int displayWidth = 1920, displayHeight = 1080;
                const int display = getDisplayIndex ? getDisplayIndex(window) : 0;
                Rect bounds{};
                DisplayMode mode{};
                int displayX = 0, displayY = 0;
                if (display >= 0 && getDisplayBounds && getDisplayBounds(display, &bounds) == 0)
                {
                    displayX = bounds.x;
                    displayY = bounds.y;
                    if (bounds.w > 0 && bounds.h > 0)
                    {
                        displayWidth = bounds.w;
                        displayHeight = bounds.h;
                    }
                }
                else if (display >= 0 && getDesktopMode && getDesktopMode(display, &mode) == 0 && mode.w > 0 && mode.h > 0)
                {
                    displayWidth = mode.w;
                    displayHeight = mode.h;
                }
                setSize(window, displayWidth, displayHeight);
                setPosition(window, displayX, displayY);
            }
            else
            {
                setFullscreen(window, 0);
                setBordered(window, 1);
                if (savedWindowState)
                {
                    setSize(window, originalWidth, originalHeight);
                    setPosition(window, originalX, originalY);
                    if (originalFullscreen) setFullscreen(window, 0x1001u);
                }
            }
            SaveConfig();
        }

        void SetUnlock(bool enable)
        {
            if (enable && !swapIntervalOverridden && getSwapInterval && setSwapInterval)
            {
                originalSwapInterval = getSwapInterval();
                setSwapInterval(0);
                swapIntervalOverridden = true;
            }
            else if (!enable && swapIntervalOverridden && setSwapInterval)
            {
                setSwapInterval(originalSwapInterval);
                swapIntervalOverridden = false;
            }
            unlockFps = enable;
            nextFrame = std::chrono::steady_clock::now();
            SaveConfig();
        }

        void __cdecl SwapWindowHook(void *window)
        {
            subhook::ScopedHookRemove remove(swapHook);
            // The first swap is guaranteed to run with SDL's GL context
            // current, unlike early client initialization.
            if (unlockFps && !swapIntervalOverridden) SetUnlock(true);
            if (!unlockFps && maximumFps > 0)
            {
                using clock = std::chrono::steady_clock;
                const auto interval = std::chrono::duration_cast<clock::duration>(
                    std::chrono::duration<double>(1.0 / maximumFps));
                const auto now = clock::now();
                if (nextFrame == clock::time_point{}) nextFrame = now;
                nextFrame += interval;
                if (nextFrame < now - interval * 3) nextFrame = now;
                if (nextFrame > now) WaitUntil(nextFrame);
            }
            const bool inGame = addresses::IsInGame.ptr && addresses::IsInGame.ptr->b1;
            const bool hideSeek = inGame && flags::Get("hs_active") == "1";
            const bool inLobby = hideSeek && flags::Get("hs_phase") == "lobby";
            static bool lobbyCursorActive = false;
            if (inLobby)
            {
                if (setRelativeMouseMode) setRelativeMouseMode(0);
                if (showCursor) showCursor(1);
                lobbyCursorActive = true;
            }
            else if (lobbyCursorActive)
            {
                // Leaving the lobby must ALWAYS take the OS cursor back away —
                // including disconnecting to the main menu, where the old
                // in-game-only guard left the Windows cursor stuck on screen.
                if (showCursor) showCursor(0);
                if (inGame && (!addresses::MenuTypeID.ptr || *addresses::MenuTypeID == 0))
                {
                    if (setRelativeMouseMode) setRelativeMouseMode(1);
                }
                lobbyCursorActive = false;
            }
            // Draw the custom game overlay after every native UI pass and
            // immediately before presentation.
            if (hideSeek && (!addresses::MenuTypeID.ptr || *addresses::MenuTypeID == 0))
            {
                api::BeginLuaDrawing();
                GetMainLuaManager()->CallHooks("DrawOverlay", "post");
                api::EndLuaDrawing();
                api::FlushImageLayer(true);
                api::FlushQueuedTexts();
            }
            originalSwap(window);
        }

        int __cdecl SetFullscreenHook(void *window, unsigned int flags)
        {
            subhook::ScopedHookRemove remove(fullscreenHook);
            // Sub Rosa calls SDL_SetWindowFullscreen when a display-sized
            // borderless window regains focus. Keep it windowed while the
            // user's borderless mode is active.
            if (borderless && flags != 0)
                return 0;
            return originalSetFullscreen(window, flags);
        }
    }

    void Install()
    {
#if _WIN32
        if (initialized) return;
        LoadConfig();
        sdl = GetModuleHandleA("SDL2.dll");
        if (!sdl) return;
        showCursor = reinterpret_cast<ShowCursor>(GetProcAddress(sdl, "SDL_ShowCursor"));
        setRelativeMouseMode = reinterpret_cast<SetRelativeMouseMode>(
            GetProcAddress(sdl, "SDL_SetRelativeMouseMode"));
        frameTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
        originalSwap = reinterpret_cast<SwapWindow>(GetProcAddress(sdl, "SDL_GL_SwapWindow"));
        setFullscreen = reinterpret_cast<SetFullscreen>(GetProcAddress(sdl, "SDL_SetWindowFullscreen"));
        originalSetFullscreen = setFullscreen;
        setBordered = reinterpret_cast<SetBordered>(GetProcAddress(sdl, "SDL_SetWindowBordered"));
        setSize = reinterpret_cast<SetWindowSize>(GetProcAddress(sdl, "SDL_SetWindowSize"));
        setPosition = reinterpret_cast<SetWindowPosition>(GetProcAddress(sdl, "SDL_SetWindowPosition"));
        getSize = reinterpret_cast<GetWindowSize>(GetProcAddress(sdl, "SDL_GetWindowSize"));
        getPosition = reinterpret_cast<GetWindowPosition>(GetProcAddress(sdl, "SDL_GetWindowPosition"));
        getFlags = reinterpret_cast<GetWindowFlags>(GetProcAddress(sdl, "SDL_GetWindowFlags"));
        getDisplayIndex = reinterpret_cast<GetDisplayIndex>(GetProcAddress(sdl, "SDL_GetWindowDisplayIndex"));
        getDisplayBounds = reinterpret_cast<GetDisplayBounds>(GetProcAddress(sdl, "SDL_GetDisplayBounds"));
        getDesktopMode = reinterpret_cast<GetDesktopDisplayMode>(GetProcAddress(sdl, "SDL_GetDesktopDisplayMode"));
        setSwapInterval = reinterpret_cast<SetSwapInterval>(GetProcAddress(sdl, "SDL_GL_SetSwapInterval"));
        getSwapInterval = reinterpret_cast<GetSwapInterval>(GetProcAddress(sdl, "SDL_GL_GetSwapInterval"));
        if (originalSwap)
        {
            swapHook = new subhook::Hook(reinterpret_cast<void *>(originalSwap),
                reinterpret_cast<void *>(&SwapWindowHook), subhook::HookFlag64BitOffset);
            swapHook->Install();
        }
        if (originalSetFullscreen)
        {
            fullscreenHook = new subhook::Hook(reinterpret_cast<void *>(originalSetFullscreen),
                reinterpret_cast<void *>(&SetFullscreenHook), subhook::HookFlag64BitOffset);
            fullscreenHook->Install();
        }
        initialized = true;
        if (borderless) ApplyBorderless(true);
#endif
    }

    void Draw()
    {
#if _WIN32
        if (!initialized || !addresses::DrawMenuToggleFunc.ptr ||
            *addresses::MenuTypeID != 3 || *addresses::MenuOptionsSectionID != 1) return;
        LoadConfig();
        *addresses::NextMenuButtonPositionX = 4.0f;
        *addresses::NextMenuButtonSizeX = 240.0f;
        *addresses::NextMenuButtonSizeY = 32.0f;
        *addresses::NextMenuButtonKey = static_cast<SDL_Scancode>(-1);
        *addresses::NextMenuButtonPositionY = 320.0f;
        const int oldBorderless = borderless ? 1 : 0;
        int borderlessValue = oldBorderless;
        if (addresses::DrawMenuToggleFunc("Borderless window", &borderlessValue))
            ApplyBorderless(borderlessValue != 0);

        *addresses::NextMenuButtonPositionY = 356.0f;
        const int oldUnlock = unlockFps ? 1 : 0;
        int unlockValue = oldUnlock;
        if (addresses::DrawMenuToggleFunc("Unlock FPS", &unlockValue))
            SetUnlock(unlockValue != 0);

        *addresses::NextMenuButtonPositionY = 392.0f;
        const int oldFps = maximumFps;
        addresses::DrawMenuSliderFunc("Maximum FPS", &maximumFps, 30, 360, 5);
        maximumFps = std::clamp(maximumFps, 30, 360);
        if (oldFps != maximumFps) SaveConfig();
#endif
    }
#else
    void Install() {}
    void Draw() {}
#endif
}
