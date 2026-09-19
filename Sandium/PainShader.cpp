#include "PainShader.hpp"

#include "Addresses.hpp"
#include "BrokenBones.hpp"
#include "structs/Human.hpp"
#include "api/Image.hpp"
#include "api/Text.hpp"

#if _WIN32
#undef DrawText
#endif

#include <glad/glad.h>

#include <cmath>
#include <chrono>
#include <cstdio>
#include <string>

// Pain overlay. The vignette is a procedurally generated radial alpha
// gradient (white, tinted red at draw time through the image layer's color
// modulation). Intensity =
//   chronic: total-health based, ramps in slowly as the human gets hurt
//   acute:   spikes on every health drop this frame, decays over ~1s
//   pulse:   slow heartbeat wobble once the chronic level is high

namespace painshader
{
    namespace
    {
        bool textureReady = false;
        bool textureFailed = false;
        unsigned int texture = 0;

        int lastHealth = 100;
        int lastLimbs[6] = {100, 100, 100, 100, 100, 100};
        bool limbsValid = false;
        float acute = 0.0f;
        bool hadHuman = false;
        float lastPain = 0.0f; // 0..10

        const auto started = std::chrono::steady_clock::now();

        float SmoothStep(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        void BuildTexture()
        {
            constexpr int SIZE = 128;
            unsigned char pixels[SIZE * SIZE * 4];
            const float center = (SIZE - 1) * 0.5f;
            for (int y = 0; y < SIZE; ++y)
            {
                for (int x = 0; x < SIZE; ++x)
                {
                    const float dx = x - center, dy = y - center;
                    const float d = std::sqrt(dx * dx + dy * dy) / center; // 0 center .. ~1.41 corner
                    const float a = SmoothStep(0.55f, 1.05f, d);
                    unsigned char *out = pixels + (y * SIZE + x) * 4;
                    out[0] = out[1] = out[2] = 255;
                    out[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
                }
            }
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            textureReady = true;
        }

        structs::Human *LocalHuman()
        {
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
                if (addresses::Humans[h].isActive.b1)
                    return &addresses::Humans[h];
            return nullptr;
        }
    }

    float PainLevel()
    {
        return lastPain;
    }

    void Update()
    {
#if _WIN32
        structs::Human *human = LocalHuman();
        if (!human)
        {
            hadHuman = false;
            acute = 0.0f;
            lastPain = 0.0f;
            return;
        }

        const int limbs[6] = {human->torsoHealth, human->headHealth,
                              human->leftArmHealth, human->rightArmHealth,
                              human->leftLegHealth, human->rightLegHealth};

        if (hadHuman && limbsValid)
        {
            // acute pain from any health drop since the last frame
            int dropped = lastHealth - human->health;
            for (int i = 0; i < 6; ++i)
            {
                const int limbDrop = lastLimbs[i] - limbs[i];
                if (limbDrop > 0)
                    dropped += limbDrop;
            }
            if (dropped > 0)
                acute = std::min(0.55f, acute + dropped * 0.02f);
        }
        lastHealth = human->health;
        for (int i = 0; i < 6; ++i)
            lastLimbs[i] = limbs[i];
        limbsValid = true;
        hadHuman = true;

        acute *= 0.90f;
        if (acute < 0.01f)
            acute = 0.0f;

        // Pain scale 0..10: broken bones dominate (+1.67 each), heavy total
        // damage adds up to +3, a fresh hit adds its acute burst.
        const float broken = static_cast<float>(brokenbones::BrokenCount());
        float pain = broken * 1.67f;
        pain += (1.0f - std::clamp(human->health, 0, 100) / 100.0f) * 3.0f;
        pain += acute;
        lastPain = std::clamp(pain, 0.0f, 10.0f);
#endif
    }

    void Draw()
    {
#if _WIN32
        if (!textureReady)
        {
            if (!textureFailed)
            {
                try
                {
                    BuildTexture();
                }
                catch (...)
                {
                    textureFailed = true;
                }
            }
            if (!textureReady)
                return;
        }

        // red intensity from the pain scale: 0 = none, 2 = slight, 10 = max
        const float level = lastPain;
        const float intensity = std::pow(level / 10.0f, 1.3f) * 0.85f;
        if (intensity < 0.02f)
            return;

        const float seconds = std::chrono::duration<float>(
                                  std::chrono::steady_clock::now() - started).count();
        const float pulse = level > 4.0f ? std::sin(seconds * 2.6f) * 0.08f * (level / 10.0f) : 0.0f;

        api::QueueDraw(texture, 0.0f, 0.0f, 1024.0f, 768.0f,
                       1.0f, 0.10f, 0.10f, std::clamp(intensity + pulse, 0.0f, 0.9f), 1, 0.0f);

        // the pain stat itself (user-visible in practice mode)
        char stat[64];
        std::snprintf(stat, sizeof(stat), "pain: %d", static_cast<int>(level + 0.5f));
        api::QueueText(std::string(stat), 90.0f, 740.0f, 15.0f,
                       glm::vec4(1.0f, 0.3f, 0.25f, 1.0f), api::TextAlignment::Left, true);
#endif
    }

    float Intensity()
    {
        return std::clamp(lastPain / 10.0f, 0.0f, 1.0f);
    }
}
