#include "BrokenBones.hpp"

#include "api/Sound.hpp"

#include "Addresses.hpp"
#include "ServerMedia.hpp"
#include "structs/Human.hpp"

#include <chrono>
#include <cstdio>

#if _WIN32
#include <Windows.h>
#endif

// Broken bones: a single-tick limb health drop of BREAK_DELTA or more means
// the limb took a violent impact (steep fall, car crash). The limb snaps to
// zero -- the vanilla renderer paints fully-damaged parts black -- and the
// bone-crack sound plays. The server's Bones plugin performs the same break
// authoritatively (HumanDamage hook); multiplayer clients see the zeroed
// health arrive through normal sync and the same detector fires here.

namespace brokenbones
{
    namespace
    {
        constexpr int BREAK_DELTA = 15;
        constexpr int SOUND_COOLDOWN_MS = 250;

        struct LimbRef
        {
            int *current;
            int last;          // reference at the start of the damage window
            int windowMs;      // ms since the window started
        };

        // Damage often arrives as several small hits across physics ticks
        // (ragdoll settling on a landing), so the break triggers on the total
        // drop within a short window instead of a single frame.
        LimbRef limbRefs[structs::Human::VanillaCount][6] = {};
        bool initialized[structs::Human::VanillaCount] = {};
        // broken bones do not heal: pinned at zero until the human despawns
        bool brokenLimbs[structs::Human::VanillaCount][6] = {};
        constexpr int WINDOW_MS = 400;

        unsigned int diagFrames = 0;

        std::shared_ptr<api::Sound> crackSound;
        bool soundFailed = false;
        std::chrono::steady_clock::time_point lastCrack{};

        void PlayCrack()
        {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCrack).count() < SOUND_COOLDOWN_MS)
                return;
            if (!soundFailed && !crackSound)
            {
                if (!servermedia::Ready("bonecrack.mp3"))
                    return;
                const std::string path = servermedia::Path("bonecrack.mp3");
                if (path.empty())
                    return;
                try
                {
                    crackSound = api::Sound::Load(path);
                }
                catch (...)
                {
                    soundFailed = true;
                    return;
                }
            }
            if (crackSound)
            {
                crackSound->Play();
                lastCrack = now;
            }
        }
    }

    void Update()
    {
        // periodic ground-truth dump of the local human's limb HP
        if (++diagFrames % 120 == 0)
        {
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                if (!addresses::Humans[h].isActive.b1)
                    continue;
                if (std::FILE *f = std::fopen("sandium_bones.txt", "w"))
                {
                    structs::Human &hm = addresses::Humans[h];
                    std::fprintf(f, "head=%d torso=%d lArm=%d rArm=%d lLeg=%d rLeg=%d total=%d\n",
                                 hm.headHealth, hm.torsoHealth, hm.leftArmHealth,
                                 hm.rightArmHealth, hm.leftLegHealth, hm.rightLegHealth,
                                 hm.health);
                    std::fclose(f);
                }
                break; // first active human (local in practice)
            }
        }
        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
        {
            structs::Human &human = addresses::Humans[h];
            if (!human.isActive.b1)
            {
                initialized[h] = false;
                for (int i = 0; i < 6; ++i)
                    brokenLimbs[h][i] = false;
                continue;
            }

            // keep broken limbs at zero even through the game's health regen
            for (int i = 0; i < 6; ++i)
                if (brokenLimbs[h][i])
                    *limbRefs[h][i].current = 0;
            int *limbs[6] = {&human.headHealth, &human.torsoHealth, &human.leftArmHealth,
                             &human.rightArmHealth, &human.leftLegHealth, &human.rightLegHealth};

            if (!initialized[h])
            {
                for (int i = 0; i < 6; ++i)
                    limbRefs[h][i] = {limbs[i], *limbs[i], 0};
                initialized[h] = true;
                continue;
            }

            for (int i = 0; i < 6; ++i)
            {
                if (brokenLimbs[h][i])
                    continue;
                LimbRef &ref = limbRefs[h][i];
                const int value = *limbs[i];
                if (value > ref.last)
                {
                    // healed/reset: restart the window from the new value
                    ref.last = value;
                    ref.windowMs = 0;
                    continue;
                }
                if (value == ref.last)
                {
                    ref.windowMs = 0;
                    continue;
                }

                const int delta = ref.last - value;
                if (delta >= BREAK_DELTA)
                {
                    *limbs[i] = 0; // broken: renders black (practice sticks, MP confirms via server)
                    brokenLimbs[h][i] = true;
                    PlayCrack();
                    ref.last = 0;
                    ref.windowMs = 0;
                    continue;
                }

                ref.windowMs += 16; // ~one frame
                if (ref.windowMs > WINDOW_MS)
                {
                    // small damage spread too long to be one impact
                    ref.last = value;
                    ref.windowMs = 0;
                }
            }
        }
    }
}
