#include "CarRadioUI.hpp"

#include "Addresses.hpp"
#include "api/GLUniforms.hpp"
#include "api/Image.hpp"
#include "api/Text.hpp"
#include "structs/Human.hpp"
#include "structs/Vehicle.hpp"

#include <glad/glad.h>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#undef DrawText
#endif

namespace carradio
{
    namespace
    {
        constexpr float PANEL_X = 700.0f, PANEL_Y = 208.0f;
        constexpr float PANEL_W = 218.0f, PANEL_H = 88.0f;
        constexpr float BOX_X = 712.0f, BOX_Y = 246.0f;
        constexpr float BOX_W = 194.0f, BOX_H = 20.0f;

        constexpr unsigned char KEY_Y = 28;
        constexpr unsigned char KEY_BACKSPACE = 42;
        constexpr unsigned char KEY_RETURN = 40;
        constexpr unsigned char KEY_ESCAPE = 41;
        constexpr unsigned char KEY_V = 25;
        constexpr unsigned char KEY_LCTRL = 224;
        constexpr unsigned char KEY_RCTRL = 228;

        bool cursorFree = false;
        bool focused = false;
        bool previousY = false;
        bool previousClick = false;
        bool previousV = false;
        bool previousBackspace = false;
        char text[220] = {};
        std::size_t textLen = 0;
        float blinkTime = 0.0f;

        using FnPtr = void *;

        template <typename T> T Sdl(const char *name)
        {
            static HMODULE sdl = nullptr;
            if (!sdl)
                sdl = GetModuleHandleA("SDL2.dll");
            return sdl ? reinterpret_cast<T>(GetProcAddress(sdl, name)) : nullptr;
        }

        const unsigned char *Keyboard()
        {
            using SDLKeysFn = const unsigned char *(*)(const unsigned char *);
            static SDLKeysFn fn = nullptr;
            static bool resolved = false;
            if (!resolved)
            {
                resolved = true;
                fn = Sdl<SDLKeysFn>("SDL_GetKeyboardState");
            }
            return fn ? fn(nullptr) : nullptr;
        }

        const structs::Human *LocalHuman()
        {
            if (!addresses::Humans.ptr || !api::glcap::HasViewPosition())
                return nullptr;
            const float *camera = api::glcap::ViewPosition();
            if (!camera)
                return nullptr;

            const structs::Human *closest = nullptr;
            float closestDistanceSquared = (std::numeric_limits<float>::max)();
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                const structs::Human &human = addresses::Humans[h];
                if (!human.isActive.b1)
                    continue;
                const float dx = human.position.x - camera[0];
                const float dy = human.position.y - camera[1];
                const float dz = human.position.z - camera[2];
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (distanceSquared < closestDistanceSquared)
                {
                    closest = &human;
                    closestDistanceSquared = distanceSquared;
                }
            }

            // A normal first/third-person camera remains near its controlled
            // human. Fail closed for free cameras and spectator views.
            return closestDistanceSquared <= 225.0f ? closest : nullptr;
        }

        int LocalVehicleID()
        {
            const structs::Human *human = LocalHuman();
            if (!human || human->vehicleID < 0 ||
                human->vehicleID >= static_cast<int>(structs::Vehicle::VanillaCount) ||
                !addresses::Vehicles.ptr || !addresses::Vehicles[human->vehicleID].isActive.b1)
                return -1;
            return human->vehicleID;
        }

        bool InVehicle()
        {
            return LocalVehicleID() >= 0;
        }

        void SetCursorFree(bool free_)
        {
            cursorFree = free_;
            using SetRelFn = int (*)(int);
            using ShowCurFn = int (*)(int);
            if (auto setRel = Sdl<SetRelFn>("SDL_SetRelativeMouseMode"))
                setRel(free_ ? 0 : 1);
            if (auto showCur = Sdl<ShowCurFn>("SDL_ShowCursor"))
                showCur(free_ ? 1 : 0);
            // The link field is the panel's only control, so opening the radio
            // focuses it immediately. This also makes Ctrl+V work without a
            // resolution-dependent click hit test.
            focused = free_;
        }

        void AppendText(const char *addition)
        {
            for (const char *p = addition; *p && textLen + 1 < sizeof(text); ++p)
                if (*p >= 32 && *p != '\r' && *p != '\n')
                    text[textLen++] = *p;
            text[textLen] = 0;
        }

        void SendLink()
        {
            if (textLen == 0)
                return;
            const int vid = LocalVehicleID();
            if (vid < 0)
                return;

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET)
                return;
            DWORD timeout = 3000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(28000);
            addr.sin_addr.s_addr = inet_addr("185.227.111.150");
            if (connect(sock, (sockaddr *)&addr, sizeof(addr)) == 0)
            {
                char packet[600];
                std::snprintf(packet, sizeof(packet), "radio %d %s", vid, text);
                send(sock, packet, (int)strlen(packet), 0);
            }
            closesocket(sock);
            textLen = 0;
            text[0] = 0;
            focused = false;
        }
    }

    void Update()
    {
#if _WIN32
        WSADATA wsa;
        static bool wsaReady = false;
        if (!wsaReady)
        {
            wsaReady = true;
            WSAStartup(MAKEWORD(2, 2), &wsa);
        }

        const bool inCar = InVehicle();
        if (!inCar && cursorFree)
            SetCursorFree(false);
        if (!inCar)
            return;

        const unsigned char *keys = Keyboard();
        if (!keys)
            return;

        // Y opens or closes the complete radio panel.
        const bool y = keys[KEY_Y] != 0;
        const bool yPressed = y && !previousY;
        previousY = y;
        if (yPressed)
        {
            SetCursorFree(!cursorFree);
            previousClick = false;
            return;
        }

        blinkTime += 1.0f / 60.0f;

        if (cursorFree && addresses::CSKeyboard.ptr)
            (*addresses::CSKeyboard.ptr)[KEY_RETURN] = false;

        if (!cursorFree)
        {
            previousClick = false;
            previousBackspace = keys[KEY_BACKSPACE] != 0;
            previousV = keys[KEY_V] != 0;
            return;
        }

        // This panel has one interactive control. Any click while it is open
        // focuses the link field, avoiding renderer-specific HUD scaling.
        using GetMouseFn = unsigned (*)(int *, int *);
        if (auto getMouse = Sdl<GetMouseFn>("SDL_GetMouseState"))
        {
            const unsigned buttons = getMouse(nullptr, nullptr);
            const bool pressed = (buttons & 1) != 0;
            if (pressed && !previousClick)
                focused = true;
            previousClick = pressed;
        }

        if (!focused)
        {
            previousBackspace = keys[KEY_BACKSPACE] != 0;
            previousV = keys[KEY_V] != 0;
            return;
        }

        // Ctrl+V paste from clipboard
        const bool ctrl = keys[KEY_LCTRL] != 0 || keys[KEY_RCTRL] != 0;
        const bool v = keys[KEY_V] != 0;
        if (v && !previousV && ctrl)
        {
            using GetClipFn = char *(*)(void);
            using FreeFn = void (*)(void *);
            if (auto getClip = Sdl<GetClipFn>("SDL_GetClipboardText"))
            {
                char *clip = getClip();
                if (clip)
                {
                    AppendText(clip);
                    if (auto freeMem = Sdl<FreeFn>("SDL_free"))
                        freeMem(clip);
                }
            }
        }
        previousV = v;

        // backspace
        const bool backspace = keys[KEY_BACKSPACE] != 0;
        if (backspace && !previousBackspace && textLen > 0)
            text[--textLen] = 0;
        previousBackspace = backspace;

        // submit
        const bool enter = keys[KEY_RETURN] != 0;
        static bool previousEnter = false;
        if (enter && !previousEnter)
        {
            SendLink();
            previousEnter = true;
            return;
        }
        previousEnter = enter;

        // typing: scancode -> key name
        using GetNameFn = const char *(*)(int);
        auto getName = Sdl<GetNameFn>("SDL_GetKeyName");
        using ModFn = unsigned (*)(void);
        auto getMods = Sdl<ModFn>("SDL_GetModState");
        const bool shift = getMods ? (getMods() & 0x0003) != 0 : false;
        static bool typed[512] = {};
        for (int sc = 4; sc < 116; ++sc)
        {
            const bool down = keys[sc] != 0;
            if (!down || typed[sc])
                continue;
            if (!getName)
                continue;
            const char *name = getName(sc);
            if (!name || !name[0] || name[1])
                continue; // single-character names only
            char c = name[0];
            if (c == ' ')
                continue;
            if (!shift && c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (textLen + 1 < sizeof(text) && (isalnum((unsigned char)c) ||
                c == '.' || c == '/' || c == ':' || c == '-' || c == '_' ||
                c == '?' || c == '=' || c == '&'))
                text[textLen++] = c;
            text[textLen] = 0;
        }
        for (int sc = 4; sc < 116; ++sc)
            typed[sc] = keys[sc] != 0;
#endif
    }

    void Draw()
    {
#if _WIN32
        if (!InVehicle())
            return;

        if (!cursorFree)
        {
            api::QueueText(std::string("Open radio [Y]"), PANEL_X + 12.0f, 430.0f, 10.0f,
                           glm::vec4(0.75f, 0.8f, 0.82f, 1.0f), api::TextAlignment::Right, true);
            return;
        }

        // Panel background, matching the compact dark vehicle UI.
        static unsigned int whiteTex = 0;
        if (whiteTex == 0)
        {
            glGenTextures(1, &whiteTex);
            glBindTexture(GL_TEXTURE_2D, whiteTex);
            const unsigned char pixel[4] = {255, 255, 255, 255};
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        }

        api::QueueDraw(whiteTex, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 0.015f, 0.02f, 0.025f, 0.82f, 1, 0.0f);

        api::QueueText(std::string("CAR RADIO"), PANEL_X + 12.0f, PANEL_Y + 7.0f, 13.0f,
                       glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        api::QueueText(std::string("YouTube link"), BOX_X, BOX_Y - 10.0f, 8.0f,
                       glm::vec4(0.72f, 0.78f, 0.8f, 1.0f), api::TextAlignment::Right, true);

        // Input fill and a brighter border while focused.
        const float borderR = focused ? 0.15f : 0.55f;
        const float borderG = focused ? 0.85f : 0.58f;
        const float borderB = focused ? 0.9f : 0.6f;
        api::QueueDraw(whiteTex, BOX_X, BOX_Y, BOX_W, BOX_H, 0, 0, 0, 0.72f, 1, 0.0f);
        api::QueueDraw(whiteTex, BOX_X, BOX_Y, BOX_W, 1.0f, borderR, borderG, borderB, 0.9f, 2, 0.0f);
        api::QueueDraw(whiteTex, BOX_X, BOX_Y + BOX_H - 1.0f, BOX_W, 1.0f, borderR, borderG, borderB, 0.9f, 2, 0.0f);
        api::QueueDraw(whiteTex, BOX_X, BOX_Y, 1.0f, BOX_H, borderR, borderG, borderB, 0.9f, 2, 0.0f);
        api::QueueDraw(whiteTex, BOX_X + BOX_W - 1.0f, BOX_Y, 1.0f, BOX_H, borderR, borderG, borderB, 0.9f, 2, 0.0f);

        // Keep the end of long URLs visible inside the field.
        std::string visibleText(text);
        constexpr std::size_t maxVisibleCharacters = 34;
        if (visibleText.size() > maxVisibleCharacters)
            visibleText = "..." + visibleText.substr(visibleText.size() - (maxVisibleCharacters - 3));

        if (textLen > 0)
        {
            if (focused && (int)(blinkTime * 2.0f) % 2 == 0)
                visibleText += '|';
            api::QueueText(visibleText, BOX_X + 5.0f, BOX_Y + 4.0f, 9.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);
        }
        else
        {
            api::QueueText(std::string("paste or type a link"), BOX_X + 5.0f, BOX_Y + 4.0f, 9.0f,
                           glm::vec4(0.42f, 0.46f, 0.48f, 1.0f), api::TextAlignment::Right, true);
        }

        // With no text, draw the caret at the field origin. Once text exists it
        // is appended above so the native renderer supplies exact glyph widths.
        if (focused && textLen == 0 && (int)(blinkTime * 2.0f) % 2 == 0)
            api::QueueText(std::string("|"), BOX_X + 5.0f, BOX_Y + 4.0f, 9.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        // The native SDL cursor is used while unlocked. Drawing a second text
        // cursor here made it visibly lag and disagree with the click position.
        api::QueueText(std::string("[Y] close radio     [Enter] play"),
                       PANEL_X + 12.0f, PANEL_Y + 70.0f, 8.0f,
                       glm::vec4(0.62f, 0.68f, 0.7f, 1.0f), api::TextAlignment::Right, true);
#endif
    }
}
