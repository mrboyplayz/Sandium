#include "CarRadioUI.hpp"

#include "Addresses.hpp"
#include "api/Text.hpp"
#include "structs/Human.hpp"

#include <glad/glad.h>

#include <cstdio>
#include <cstring>
#include <string>

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#undef DrawText
#endif

// In-car radio UI. All rects are drawn in REAL viewport pixels through a
// tiny GL quad program, and text is queued in the game's 1024x768 text
// space converted from the same real pixels, so the panel, box, typed text
// and cursor always line up regardless of the window size.

namespace carradio
{
    namespace
    {
        constexpr unsigned char KEY_Y = 28;
        constexpr unsigned char KEY_BACKSPACE = 42;
        constexpr unsigned char KEY_RETURN = 40;
        constexpr unsigned char KEY_V = 25;
        constexpr unsigned char KEY_LCTRL = 224;
        constexpr unsigned char KEY_RCTRL = 228;
        constexpr unsigned char KEY_LMB = 7;

        bool cursorFree = false;
        bool focused = false;
        bool previousY = false;
        bool previousClick = false;
        bool previousV = false;
        bool previousBackspace = false;
        bool previousEnter = false;
        char text[220] = {};
        int textLen = 0;
        float blinkTime = 0.0f;

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

        bool InVehicle(int &vehicleID)
        {
            vehicleID = -1;
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                if (!addresses::Humans[h].isActive.b1)
                    continue;
                vehicleID = addresses::Humans[h].vehicleID;
                return vehicleID >= 0;
            }
            return false;
        }

        void SetCursorFree(bool free_)
        {
            cursorFree = free_;
            focused = false;
            using SetRelFn = int (*)(int);
            using ShowCurFn = int (*)(int);
            if (auto setRel = Sdl<SetRelFn>("SDL_SetRelativeMouseMode"))
                setRel(free_ ? 0 : 1);
            if (auto showCur = Sdl<ShowCurFn>("SDL_ShowCursor"))
                showCur(free_ ? 1 : 0);
        }

        // ---- tiny GL quad pass in viewport pixels ----
        GLuint quadProgram = 0;
        GLuint quadVao = 0;
        GLint quadRect = -1, quadColor = -1, quadRes = -1;
        bool quadReady = false;
        bool quadFailed = false;
        unsigned int whiteTex = 0;

        GLuint QuadCompile(GLenum type, const char *source)
        {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint okay = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
            if (!okay)
            {
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        bool EnsureQuad()
        {
            if (quadReady)
                return true;
            if (quadFailed)
                return false;
            const char *vs = "#version 330 core\n"
                             "uniform vec2 uRes;\n"
                             "uniform vec4 uRect; // x y w h in pixels\n"
                             "out vec2 vUv;\n"
                             "void main(){\n"
                             "  int i = gl_VertexID;\n"
                             "  float cx = (i == 1 || i == 2 || i == 4) ? 1.0 : 0.0;\n"
                             "  float cy = (i == 2 || i == 4 || i == 5) ? 1.0 : 0.0;\n"
                             "  vUv = vec2(cx, cy);\n"
                             "  vec2 pos = uRect.xy + vec2(cx, cy) * uRect.zw;\n"
                             "  gl_Position = vec4(pos / uRes * 2.0 - 1.0, 0.0, 1.0);\n"
                             "}\n";
            const char *fs = "#version 330 core\n"
                             "in vec2 vUv;\n"
                             "uniform vec4 uColor;\n"
                             "out vec4 color;\n"
                             "void main(){ color = uColor; }\n";
            GLuint v = QuadCompile(GL_VERTEX_SHADER, vs);
            GLuint f = QuadCompile(GL_FRAGMENT_SHADER, fs);
            if (!v || !f)
            {
                quadFailed = true;
                return false;
            }
            quadProgram = glCreateProgram();
            glAttachShader(quadProgram, v);
            glAttachShader(quadProgram, f);
            glLinkProgram(quadProgram);
            glDeleteShader(v);
            glDeleteShader(f);
            GLint linked = 0;
            glGetProgramiv(quadProgram, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                quadFailed = true;
                return false;
            }
            quadRect = glGetUniformLocation(quadProgram, "uRect");
            quadColor = glGetUniformLocation(quadProgram, "uColor");
            quadRes = glGetUniformLocation(quadProgram, "uRes");
            glGenVertexArrays(1, &quadVao);
            glGenTextures(1, &whiteTex);
            glBindTexture(GL_TEXTURE_2D, whiteTex);
            const unsigned char pixel[4] = {255, 255, 255, 255};
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            quadReady = true;
            return true;
        }

        int viewportWidth()
        {
            GLint v[4] = {};
            glGetIntegerv(GL_VIEWPORT, v);
            return v[2];
        }

        int viewportHeight()
        {
            GLint v[4] = {};
            glGetIntegerv(GL_VIEWPORT, v);
            return v[3];
        }

        // filled quad in viewport pixels (y from top)
        void Quad(GLuint program, GLint res, GLint rectLoc, GLint colorLoc,
                  float x, float y, float w, float h, float r, float g, float b, float a)
        {
            glUseProgram(program);
            glUniform2f(res, static_cast<float>(viewportWidth()), static_cast<float>(viewportHeight()));
            glUniform4f(rectLoc, x, y, w, h);
            glUniform4f(colorLoc, r, g, b, a);
            glBindVertexArray(quadVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        void SendLink(int vid)
        {
            if (textLen == 0)
                return;
            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET)
                return;
            DWORD timeout = 3000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
            sockaddr_in addr {};
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

        int vid = -1;
        const bool inCar = InVehicle(vid);
        if (!inCar && cursorFree)
            SetCursorFree(false);
        if (!inCar)
            return;

        const unsigned char *keys = Keyboard();
        if (!keys)
            return;

        // while the free cursor is up, the game must not see Enter (chat)
        if (cursorFree)
            (*addresses::CSKeyboard.ptr)[KEY_RETURN] = false;

        const bool y = keys[KEY_Y] != 0;
        const bool yPressed = y && !previousY;
        previousY = y;
        if (yPressed && !focused)
            SetCursorFree(!cursorFree);

        blinkTime += 1.0f / 60.0f;

        if (!cursorFree)
        {
            previousClick = false;
            previousBackspace = false;
            previousV = false;
            previousEnter = false;
            return;
        }

        // window size (real pixels) straight from SDL
        using GetMouseFocusFn = void *(*)(void);
        using WindowSizeFn = void (*)(void *, int *, int *);
        int winW = 1024, winH = 768;
        if (auto focus = Sdl<GetMouseFocusFn>("SDL_GetMouseFocus"))
        {
            if (auto getSize = Sdl<WindowSizeFn>("SDL_GetWindowSize"))
                getSize(focus, &winW, &winH);
        }
        if (winW <= 0 || winH <= 0)
        {
            winW = 1024;
            winH = 768;
        }

        using GetMouseFn = unsigned (*)(int *, int *);
        auto getMouse = Sdl<GetMouseFn>("SDL_GetMouseState");
        if (!getMouse)
            return;
        int mx = 0, my = 0;
        const unsigned buttons = getMouse(&mx, &my);
        const float mouseRealX = static_cast<float>(mx);
        const float mouseRealY = static_cast<float>(my);

        // panel geometry in real pixels (above the gear selector)
        const float px = winW * 0.515f;
        const float py = winH * 0.395f;
        const float pw = winW * 0.215f;
        const float ph = winH * 0.135f;
        const float bx = px + pw * 0.08f;
        const float by = py + ph * 0.66f;
        const float bw = pw * 0.84f;
        const float bh = ph * 0.24f;

        const bool click = (buttons & 1) != 0;
        if (click && !previousClick)
        {
            const bool inside = mouseRealX >= bx && mouseRealX <= bx + bw &&
                                mouseRealY >= by && mouseRealY <= by + bh;
            focused = inside;
        }
        previousClick = click;

        if (!focused)
        {
            previousBackspace = keys[KEY_BACKSPACE] != 0;
            previousV = keys[KEY_V] != 0;
            previousEnter = keys[KEY_RETURN] != 0;
            return;
        }

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
                    for (const char *p = clip; *p && textLen + 1 < (int)sizeof(text); ++p)
                        if (*p >= 32 && *p != '\r' && *p != '\n')
                            text[textLen++] = *p;
                    text[textLen] = 0;
                    if (auto freeMem = Sdl<FreeFn>("SDL_free"))
                        freeMem(clip);
                }
            }
        }
        previousV = v;

        const bool backspace = keys[KEY_BACKSPACE] != 0;
        if (backspace && !previousBackspace && textLen > 0)
            text[--textLen] = 0;
        previousBackspace = backspace;

        const bool enter = keys[KEY_RETURN] != 0;
        if (enter && !previousEnter)
        {
            SendLink(vid);
            previousEnter = enter;
            return;
        }
        previousEnter = enter;

        using GetNameFn = const char *(*)(int);
        auto getName = Sdl<GetNameFn>("SDL_GetKeyName");
        using ModFn = unsigned (*)(void);
        auto getMods = Sdl<ModFn>("SDL_GetModState");
        const bool shift = getMods ? (getMods() & 0x0003) != 0 : false;
        static bool typed[512] = {};
        for (int sc = 4; sc < 116; ++sc)
        {
            const bool down = keys[sc] != 0;
            if (!down || typed[sc] || !getName)
                continue;
            const char *name = getName(sc);
            if (!name || !name[0] || name[1])
                continue;
            char c = name[0];
            if (c == ' ')
                continue;
            if (!shift && c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (textLen + 1 < (int)sizeof(text) &&
                (isalnum((unsigned char)c) || c == '.' || c == '/' || c == ':' ||
                 c == '-' || c == '_' || c == '?' || c == '=' || c == '&'))
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
        int vid = -1;
        if (!InVehicle(vid))
            return;
        if (!EnsureQuad())
            return;

        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        if (viewport[2] <= 0 || viewport[3] <= 0)
            return;
        const float W = static_cast<float>(viewport[2]);
        const float H = static_cast<float>(viewport[3]);

        GLint previousProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
        GLint previousVao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
        GLboolean previousBlend = glIsEnabled(GL_BLEND);
        GLint previousSrc = 0, previousDst = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousSrc);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousDst);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(quadProgram);
        glUniform2f(quadRes, W, H);
        glBindVertexArray(quadVao);

        // panel
        const float px = W * 0.515f;
        const float py = H * 0.395f;
        const float pw = W * 0.215f;
        const float ph = H * 0.135f;
        const float bx = px + pw * 0.08f;
        const float by = py + ph * 0.66f;
        const float bw = pw * 0.84f;
        const float bh = ph * 0.24f;

        glUniform4f(quadRect, px, py, pw, ph);
        glUniform4f(quadColor, 0.0f, 0.0f, 0.0f, 0.55f);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // box fill + outline
        glUniform4f(quadRect, bx, by, bw, bh);
        glUniform4f(quadColor, 0.0f, 0.0f, 0.0f, 0.6f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform4f(quadColor, 1.0f, 1.0f, 1.0f, 0.7f);
        glUniform4f(quadRect, bx, by, bw, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform4f(quadRect, bx, by + bh - 1.0f, bw, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform4f(quadRect, bx, by, 1.0f, bh);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform4f(quadRect, bx + bw - 1.0f, by, 1.0f, bh);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glUseProgram(static_cast<GLuint>(previousProgram));
        glBindVertexArray(static_cast<GLuint>(previousVao));
        if (previousBlend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        glBlendFunc(static_cast<GLenum>(previousSrc), static_cast<GLenum>(previousDst));

        // text lives in the game's 1024x768 space; convert from real pixels
        const auto toTextX = [&](float realX) { return realX / W * 1024.0f; };
        const auto toTextY = [&](float realY) { return realY / H * 768.0f; };

        api::QueueText(std::string("Radio"), toTextX(px + pw * 0.04f), toTextY(py + ph * 0.06f), 16.0f,
                       glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);
        api::QueueText(std::string("youtube link"), toTextX(px + pw * 0.04f), toTextY(by + bh * 0.18f), 11.0f,
                       glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        if (textLen > 0)
            api::QueueText(std::string(text), toTextX(bx + 5.0f), toTextY(by + bh * 0.18f), 11.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);
        else if (focused)
            api::QueueText(std::string("_"), toTextX(bx + 5.0f), toTextY(by + bh * 0.18f), 11.0f,
                           glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), api::TextAlignment::Right, true);

        if (focused && (int)(blinkTime * 2.0f) % 2 == 0)
            api::QueueText(std::string("|"), toTextX(bx + 6.0f + textLen * 5.5f), toTextY(by + bh * 0.18f), 11.0f,
                           glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), api::TextAlignment::Right, true);

        if (cursorFree)
        {
            // cursor drawn from the real mouse position via the same quad pass
            using GetMouseFn = unsigned (*)(int *, int *);
            if (auto getMouse = Sdl<GetMouseFn>("SDL_GetMouseState"))
            {
                int mx = 0, my = 0;
                getMouse(&mx, &my);
                glUseProgram(quadProgram);
                glUniform2f(quadRes, W, H);
                glUniform4f(quadRect, static_cast<float>(mx), static_cast<float>(my), 14.0f, 14.0f);
                glUniform4f(quadColor, 1.0f, 1.0f, 1.0f, 0.9f);
                glBindVertexArray(quadVao);
                glBlendFunc(GL_ONE, GL_ONE);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }

        api::QueueText(std::string(cursorFree ? "[Y] lock  [Enter] play" : "[Y] radio cursor"),
                       toTextX(px + pw * 0.04f), toTextY(py + ph * 0.40f), 9.0f,
                       glm::vec4(0.65f, 0.65f, 0.65f, 1.0f), api::TextAlignment::Right, true);
#endif
    }
}
