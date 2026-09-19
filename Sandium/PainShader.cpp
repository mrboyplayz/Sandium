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
#include <cstdlib>
#include <string>

#if _WIN32
#define NOMINMAX
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <Windows.h>
extern void *SandiumMMDeviceEnumerator;
#endif

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

        // unconsciousness: maxed pain sustained, or one huge slam, knocks the
        // player out for a few seconds -- screen fades to black and the whole
        // audio session ducks to silence, then recovers.
        bool unconscious = false;
        float unconTimer = 0.0f;
        float sustainTimer = 0.0f;
        float immunity = 0.0f;
        float blackFade = 0.0f;
        bool volumeDucked = false;
        float exertion = 0.0f;
        bool exertionValid = false;
        float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;

        void SetSessionVolume(float volume)
        {
            static void *simpleAudioVolume = nullptr;
            static bool attempted = false;
            if (!attempted)
            {
                attempted = true;
                IMMDeviceEnumerator *enumerator = nullptr;
                IMMDevice *device = nullptr;
                IAudioClient *client = nullptr;
                if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                               __uuidof(IMMDeviceEnumerator),
                                               reinterpret_cast<void **>(&enumerator))) &&
                    enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                    device && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                         reinterpret_cast<void **>(&client))) &&
                    client)
                    client->GetService(__uuidof(ISimpleAudioVolume),
                                       reinterpret_cast<void **>(&simpleAudioVolume));
                if (enumerator) enumerator->Release();
                if (device) device->Release();
                if (client) client->Release();
            }
            if (simpleAudioVolume)
                reinterpret_cast<ISimpleAudioVolume *>(simpleAudioVolume)->SetMasterVolume(volume, nullptr);
        }

        void GoUnconscious(float severity) // 0..1: how brutal the knockout is
        {
            unconscious = true;
            structs::Human *koHuman = LocalHuman();
            if (koHuman)
                koHuman->movementStateID = 5; // vanilla knocked-down pose
            // deeper pain / harder slams keep you out longer, plus jitter
            unconTimer = 2.5f + severity * 5.0f + (std::rand() % 20) / 10.0f - 1.0f;
            blackFade = 0.0f;
            SetSessionVolume(0.1f); // muffled, not dead silent
            volumeDucked = true;
        }

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

    float Blur()
    {
#if _WIN32
        return std::clamp((lastPain / 10.0f - 0.7f) / 0.3f, 0.0f, 1.0f);
#else
        return 0.0f;
#endif
    }

    void Update()
    {
#if _WIN32
        structs::Human *human = LocalHuman();
        // dead (gone or health gone): every pain effect shuts off -- no
        // vignette, no grain, no shake, no black screen, audio back
        if (!human || human->health <= 0)
        {
            hadHuman = false;
            acute = 0.0f;
            lastPain = 0.0f;
            exertion = 0.0f;
            sustainTimer = 0.0f;
            unconscious = false;
            human->movementStateID = 0;
            if (volumeDucked)
            {
                SetSessionVolume(1.0f);
                volumeDucked = false;
            }
            blackFade = std::max(blackFade - 1.0f / 60.0f / 0.2f, 0.0f);
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

        // moving on broken limbs keeps hurting: pain creeps up while walking
        int brokenLegs = 0, brokenArms = 0;
        brokenbones::BrokenLimbs(brokenLegs, brokenArms);
        const float dxp = human->position.x - lastX;
        const float dyp = human->position.y - lastY;
        const float dzp = human->position.z - lastZ;
        const float moved = sqrtf(dxp * dxp + dyp * dyp + dzp * dzp);
        if (exertionValid && moved > 0.02f && moved < 2.0f)
        {
            if (brokenLegs > 0)
                exertion += moved * 0.55f * (float)brokenLegs;
            if (brokenArms > 0)
                exertion += moved * 0.20f * (float)brokenArms;
        }
        lastX = human->position.x;
        lastY = human->position.y;
        lastZ = human->position.z;
        exertionValid = true;
        exertion = std::min(exertion * 0.985f, 3.0f);

        float pain = broken * 1.67f;
        pain += (1.0f - std::clamp(human->health, 0, 100) / 100.0f) * 3.0f;
        pain += acute;
        pain += exertion;
        lastPain = std::clamp(pain, 0.0f, 10.0f);

        if (immunity > 0.0f)
            immunity -= 1.0f / 60.0f;

        if (unconscious)
        {
            human->movementStateID = 5; // stay down while out
            unconTimer -= 1.0f / 60.0f;
            blackFade = std::min(blackFade + 1.0f / 60.0f / 0.35f, 1.0f);
            if (unconTimer <= 0.0f)
            {
                unconscious = false;
                immunity = 5.0f;
                sustainTimer = 0.0f;
                if (volumeDucked)
                {
                    SetSessionVolume(0.35f); // wake up muffled, then recover
                    volumeDucked = false;
                }
            }
        }
        else if (immunity <= 0.0f)
        {
            if (lastPain >= 9.5f)
            {
                sustainTimer += 1.0f / 60.0f;
                if (sustainTimer >= 1.2f)
                    GoUnconscious(std::min(lastPain / 10.0f, 1.0f));
            }
            else
                sustainTimer = 0.0f;

            if (acute >= 0.5f)
                GoUnconscious(std::min(acute * 1.8f, 1.0f));
        }

        blackFade = unconscious ? blackFade
                                : std::max(blackFade - 1.0f / 60.0f / 0.8f, 0.0f);
#endif
    }

    int Uncon()
    {
        return unconscious ? 1 : 0;
    }

    float BlackFade()
    {
        return blackFade;
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

        // stats, top-left
        char stat[64];
        std::snprintf(stat, sizeof(stat), "pain: %d", static_cast<int>(level + 0.5f));
        api::QueueText(std::string(stat), 16.0f, 24.0f, 15.0f,
                       glm::vec4(1.0f, 0.3f, 0.25f, 1.0f), api::TextAlignment::Right, true);
        std::snprintf(stat, sizeof(stat), "uncon: %d", Uncon());
        api::QueueText(std::string(stat), 16.0f, 46.0f, 15.0f,
                       glm::vec4(0.7f, 0.7f, 0.75f, 1.0f), api::TextAlignment::Right, true);
#endif
    }

    float Intensity()
    {
        return std::clamp(lastPain / 10.0f, 0.0f, 1.0f);
    }
}
