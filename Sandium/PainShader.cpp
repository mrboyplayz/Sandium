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
        int lastHealth = 100;
        int lastLimbs[6] = {100, 100, 100, 100, 100, 100};
        bool limbsValid = false;
        float acute = 0.0f;
        bool hadHuman = false;
        float lastPain = 0.0f; // 0..10

        float SmoothStep(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
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
        // red intensity from the pain scale: 0 = none, 2 = slight, 10 = max
        const float level = lastPain;
        const float intensity = std::pow(level / 10.0f, 1.3f) * 0.85f;
        if (intensity < 0.02f)
            return;

        (void)intensity;

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
