#include "CarRadioUI.hpp"

#include "Addresses.hpp"
#include "api/Image.hpp"
#include "api/Text.hpp"
#include "structs/Human.hpp"

#include <glad/glad.h>

#include <cstring>
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
        constexpr float PANEL_X = 672.0f, PANEL_Y = 208.0f;
        constexpr float PANEL_W = 288.0f, PANEL_H = 92.0f;
        constexpr float BOX_X = 768.0f, BOX_Y = 270.0f;
        constexpr float BOX_W = 182.0f, BOX_H = 22.0f;

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
        float mouseUiX = -100.0f, mouseUiY = -100.0f;
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

        bool InVehicle()
        {
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                if (!addresses::Humans[h].isActive.b1)
                    continue;
                return addresses::Humans[h].vehicleID >= 0;
            }
            return false;
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
            focused = false;
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
            // local human's vehicle index
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                if (!addresses::Humans[h].isActive.b1)
                    continue;
                const int vid = addresses::Humans[h].vehicleID;
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
                break;
            }
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

        // Y toggles the free cursor (unless typing in the box)
        const bool y = keys[KEY_Y] != 0;
        const bool yPressed = y && !previousY;
        previousY = y;
        if (yPressed && !focused)
            SetCursorFree(!cursorFree);

        blinkTime += 1.0f / 60.0f;

        if (!cursorFree)
        {
            previousClick = false;
            previousBackspace = keys[KEY_BACKSPACE] != 0;
            previousV = keys[KEY_V] != 0;
            return;
        }

        // mouse in UI space (the layer stretches the window to 1024x768)
        using GetMouseFn = unsigned (*)(int *, int *);
        if (auto getMouse = Sdl<GetMouseFn>("SDL_GetMouseState"))
        {
            int mx = 0, my = 0;
            getMouse(&mx, &my);
            GLint viewport[4] = {};
            glGetIntegerv(GL_VIEWPORT, viewport);
            if (viewport[2] > 0 && viewport[3] > 0)
            {
                mouseUiX = static_cast<float>(mx) / viewport[2] * 1024.0f;
                mouseUiY = static_cast<float>(my) / viewport[3] * 768.0f;
            }
        }

        // click handling: focus the box
        const bool click = keys[7] != 0; // placeholder, replaced by mouse button below
        (void)click;
        int mx = 0, my = 0;
        if (auto getMouse2 = Sdl<GetMouseFn>("SDL_GetMouseState"))
        {
            const unsigned buttons = getMouse2(nullptr, &my);
            mx = (int)mouseUiX;
            my = (int)mouseUiY;
            const bool pressed = (buttons & 1) != 0;
            if (pressed && !previousClick)
            {
                const bool inside = mouseUiX >= BOX_X && mouseUiX <= BOX_X + BOX_W &&
                                    mouseUiY >= BOX_Y && mouseUiY <= BOX_Y + BOX_H;
                focused = inside;
            }
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

        // panel background, same semi-tinted black as the gear UI
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

        api::QueueDraw(whiteTex, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 0, 0, 0, 0.55f, 1, 0.0f);

        api::QueueText(std::string("Radio"), PANEL_X + 8.0f, PANEL_Y + 6.0f, 20.0f,
                       glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        api::QueueText(std::string("youtube link"), PANEL_X + 8.0f, BOX_Y + 4.0f, 13.0f,
                       glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        // box fill + white outline
        api::QueueDraw(whiteTex, BOX_X, BOX_Y, BOX_W, BOX_H, 0, 0, 0, 0.6f, 1, 0.0f);
        api::QueueDraw(whiteTex, BOX_X, BOX_Y, BOX_W, 1.0f, 1, 1, 1, 0.7f, 1, 0.0f);
        api::QueueDraw(whiteTex, BOX_X, BOX_Y + BOX_H - 1.0f, BOX_W, 1.0f, 1, 1, 1, 0.7f, 1, 0.0f);
        api::QueueDraw(whiteTex, BOX_X, BOX_Y, 1.0f, BOX_H, 1, 1, 1, 0.7f, 1, 0.0f);
        api::QueueDraw(whiteTex, BOX_X + BOX_W - 1.0f, BOX_Y, 1.0f, BOX_H, 1, 1, 1, 0.7f, 1, 0.0f);

        // typed text (or hint)
        if (textLen > 0)
        {
            api::QueueText(std::string(text), BOX_X + 5.0f, BOX_Y + 4.0f, 13.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);
        }
        else
        {
            api::QueueText(std::string(focused ? "_" : ""), BOX_X + 5.0f, BOX_Y + 4.0f, 13.0f,
                           glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), api::TextAlignment::Right, true);
        }

        // focus caret
        if (focused && (int)(blinkTime * 2.0f) % 2 == 0)
            api::QueueText(std::string("|"), BOX_X + 6.0f + textLen * 6.2f, BOX_Y + 4.0f, 13.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        // free cursor
        if (cursorFree)
            api::QueueText(std::string("+"), mouseUiX - 4.0f, mouseUiY - 6.0f, 14.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        // hint
        api::QueueText(std::string(cursorFree ? "[Y] lock mouse  [Enter] play" : "[Y] radio cursor"),
                       PANEL_X + 8.0f, PANEL_Y + PANEL_H - 22.0f, 11.0f,
                       glm::vec4(0.7f, 0.7f, 0.7f, 1.0f), api::TextAlignment::Right, true);
#endif
    }
}
