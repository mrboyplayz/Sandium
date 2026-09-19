#include "BrokenBones.hpp"

#include "api/Sound.hpp"

#include "Addresses.hpp"
#include "ServerMedia.hpp"
#include "structs/Human.hpp"

#include <chrono>

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
            int last;
        };

        LimbRef limbRefs[structs::Human::VanillaCount][6] = {};
        bool initialized[structs::Human::VanillaCount] = {};

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
        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
        {
            structs::Human &human = addresses::Humans[h];
            if (!human.isActive.b1)
            {
                initialized[h] = false;
                continue;
            }
            if (!initialized[h])
            {
                limbRefs[h][0] = {&human.headHealth, human.headHealth};
                limbRefs[h][1] = {&human.torsoHealth, human.torsoHealth};
                limbRefs[h][2] = {&human.leftArmHealth, human.leftArmHealth};
                limbRefs[h][3] = {&human.rightArmHealth, human.rightArmHealth};
                limbRefs[h][4] = {&human.leftLegHealth, human.leftLegHealth};
                limbRefs[h][5] = {&human.rightLegHealth, human.rightLegHealth};
                initialized[h] = true;
                continue;
            }

            LimbRef refs[6] = {
                {&human.headHealth, limbRefs[h][0].last},
                {&human.torsoHealth, limbRefs[h][1].last},
                {&human.leftArmHealth, limbRefs[h][2].last},
                {&human.rightArmHealth, limbRefs[h][3].last},
                {&human.leftLegHealth, limbRefs[h][4].last},
                {&human.rightLegHealth, limbRefs[h][5].last},
            };

            for (int i = 0; i < 6; ++i)
            {
                const int value = *refs[i].current;
                const int delta = refs[i].last - value;
                if (delta >= BREAK_DELTA)
                {
                    *refs[i].current = 0; // broken: renders black (practice sticks, MP confirms via server)
                    PlayCrack();
                    limbRefs[h][i].last = 0;
                    continue;
                }
                limbRefs[h][i].last = value;
            }
        }
    }
}
