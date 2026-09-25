#include "MenuBackground.hpp"

#include "api/Image.hpp"
#include "api/Logging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace menubackground
{
    namespace
    {
        constexpr float CanvasWidth = 1024.0f;
        constexpr float CanvasHeight = 768.0f;
        constexpr double SlideSeconds = 8.0;
        constexpr double FadeSeconds = 2.0;

        struct State
        {
            bool initialized = false;
            bool available = false;
            std::array<std::shared_ptr<api::Image>, 3> slides;
            std::shared_ptr<api::Image> white;
            std::chrono::steady_clock::time_point started;
        };

        State &GetState()
        {
            static State state;
            return state;
        }

        void Initialize(State &state)
        {
            state.initialized = true;
            try
            {
                state.slides = {
                    api::Image::LoadCore("sandium/models/menu_backgrounds/dear_lord.png"),
                    api::Image::LoadCore("sandium/models/menu_backgrounds/images.jpg"),
                    api::Image::LoadCore("sandium/models/menu_backgrounds/image_one.png")
                };
                state.white = api::Image::LoadCore("sandium/models/white.png");

                for (const auto &slide : state.slides)
                    if (!slide || !slide->IsValid())
                        throw std::runtime_error("a menu background texture is invalid");

                state.started = std::chrono::steady_clock::now();
                state.available = true;
                api::GetSandiumLogger()->Log("Loaded animated main-menu background ({} slides)", state.slides.size());
            }
            catch (const std::exception &error)
            {
                api::GetSandiumLogger()->Log("<red>Menu background disabled: {}", error.what());
            }
        }

        void DrawStretched(const std::shared_ptr<api::Image> &image, float alpha, int layer)
        {
            if (!image || alpha <= 0.0f)
                return;

            api::QueueDraw(image->TextureID(), 0.0f, 0.0f, CanvasWidth, CanvasHeight,
                           1.0f, 1.0f, 1.0f, alpha, layer, 0.0f);
        }
    }

    void Draw()
    {
        State &state = GetState();
        if (!state.initialized)
            Initialize(state);
        if (!state.available)
            return;

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - state.started).count();
        const std::size_t current = static_cast<std::size_t>(elapsed / SlideSeconds) % state.slides.size();
        const std::size_t next = (current + 1) % state.slides.size();
        const double withinSlide = std::fmod(elapsed, SlideSeconds);
        float fade = static_cast<float>((withinSlide - (SlideSeconds - FadeSeconds)) / FadeSeconds);
        fade = std::clamp(fade, 0.0f, 1.0f);
        fade = fade * fade * (3.0f - 2.0f * fade);

        // Keep the outgoing slide opaque and dissolve the next over it. This
        // avoids a dark pulse in the middle of each transition.
        DrawStretched(state.slides[current], 1.0f, -100);
        DrawStretched(state.slides[next], fade, -99);

        // A light veil keeps the original white text and black buttons legible
        // without changing any of the native menu widget styling.
        if (state.white && state.white->IsValid())
            api::QueueDraw(state.white->TextureID(), 0.0f, 0.0f, CanvasWidth, CanvasHeight,
                           0.0f, 0.0f, 0.0f, 0.18f, -90, 0.0f);
    }
}
