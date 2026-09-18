#pragma once

#include <glm/glm.hpp>
#include <string>

#if _WIN32
#undef DrawText
#endif

namespace api
{
    enum class TextAlignment
    {
        Right,
        Center,
        Left,
        Up,
        Down
    };

    void DrawText(const std::string &text, float x, float y, float size, const glm::vec4 &color, TextAlignment alignment = TextAlignment::Center, bool shadows = false);

    // Queued text renders on top of the game's HUD: anything drawn
    // immediately during the DrawHUD hook gets overdrawn by the game's own
    // HUD pass, so Lua UI text queues here and flushes after it.
    void QueueText(const std::string &text, float x, float y, float size, const glm::vec4 &color, TextAlignment alignment = TextAlignment::Center, bool shadows = false);
    void FlushQueuedTexts();
    void ClearQueuedTexts();
}