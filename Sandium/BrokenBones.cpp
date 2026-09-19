#include "BrokenBones.hpp"

#include "api/Sound.hpp"

#include "Addresses.hpp"
#include "ServerMedia.hpp"
#include "structs/Human.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <vector>

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

        // ---- bone drive records ----
        // On human spawn the game creates 16 consecutive 0xF4-byte bone
        // records (one per skeleton bone). Each holds a default drive
        // strength of 0.00390625 at +0x70. Zeroing a record's strength for a
        // broken bone removes the limb drive, letting it dangle.
        struct RecordSet { std::uint32_t *base; };

        bool MatchesRecord(const std::uint32_t *q)
        {
            return q[-1] == 1 && q[0] == 7 && q[27] == 0 &&
                   *reinterpret_cast<const float *>(reinterpret_cast<const char *>(q) + 0x70) == 0.00390625f;
        }

        std::vector<RecordSet> FindRecordSets()
        {
            std::vector<RecordSet> sets;
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            std::uintptr_t page = base + 0x404A6C;
            const std::uintptr_t end = base + (1ULL << 31); // state array lives within 2GB of the module base
            MEMORY_BASIC_INFORMATION mbi{};
            while (page < end)
            {
                if (!VirtualQuery(reinterpret_cast<void *>(page), &mbi, sizeof(mbi)))
                    break;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                {
                    auto p = reinterpret_cast<const std::uint32_t *>(mbi.BaseAddress);
                    const auto regionEnd = reinterpret_cast<const std::uint32_t *>(
                        reinterpret_cast<const char *>(mbi.BaseAddress) + mbi.RegionSize);
                    while (p + 4 < regionEnd)
                    {
                        if (MatchesRecord(p))
                        {
                            bool fullSet = true;
                            for (int slot = 1; slot < 16; ++slot)
                                if (!MatchesRecord(reinterpret_cast<const std::uint32_t *>(
                                        reinterpret_cast<const char *>(p) + 0xF4 * slot)))
                                {
                                    fullSet = false;
                                    break;
                                }
                            if (fullSet)
                            {
                                sets.push_back({const_cast<std::uint32_t *>(p)});
                                p += 16 * 0xF4 / 4;
                                continue;
                            }
                        }
                        ++p;
                    }
                }
                page = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            }
            return sets;
        }

        void BreakBoneDrives(int limbGroup)
        {
            // skeleton bones per limb group (enum.body)
            int bones[3];
            int boneCount = 0;
            switch (limbGroup)
            {
                case 0: bones[boneCount++] = 3; break;
                case 1: bones[boneCount++] = 0; bones[boneCount++] = 1; bones[boneCount++] = 2; break;
                case 2: bones[boneCount++] = 4; bones[boneCount++] = 5; bones[boneCount++] = 6; break;
                case 3: bones[boneCount++] = 7; bones[boneCount++] = 8; bones[boneCount++] = 9; break;
                case 4: bones[boneCount++] = 10; bones[boneCount++] = 11; bones[boneCount++] = 12; break;
                case 5: bones[boneCount++] = 13; bones[boneCount++] = 14; bones[boneCount++] = 15; break;
            }
            if (!boneCount)
                return;
            for (const RecordSet &set : FindRecordSets())
            {
                for (int slot = 0; slot < 16; ++slot)
                {
                    for (int b = 0; b < boneCount; ++b)
                    {
                        if (slot == bones[b])
                        {
                            *reinterpret_cast<float *>(reinterpret_cast<char *>(set.base) + 0xF4 * slot + 0x70) = 0.0f;
                        }
                    }
                }
            }
        }

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
        // limb group indices: 0 head, 1 torso, 2 left arm, 3 right arm, 4 left leg, 5 right leg
        void BreakLimb(std::size_t human, int limbGroup)
        {
            static const int *groupBones[6] = {
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            (void)groupBones;
            static int boneLists[6][3] = {
                {3, -1, -1}, {0, 1, 2}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}};
            structs::Human &humanRef = addresses::Humans[human];
            int *limbs[6] = {&humanRef.headHealth, &humanRef.torsoHealth, &humanRef.leftArmHealth,
                             &humanRef.rightArmHealth, &humanRef.leftLegHealth, &humanRef.rightLegHealth};
            *limbs[limbGroup] = 0;
            brokenLimbs[human][limbGroup] = true;
            limbRefs[human][limbGroup] = {limbs[limbGroup], 0};
            initialized[human] = true;
            PlayCrack();
            BreakBoneDrives(limbGroup);
        }

        // Chat test commands: typing "break arm" / "break leg" in chat breaks
        // a random side on the local human (the chat input buffer is client
        // state dword index 283785913, 60 chars).
        void CheckChatCommands()
        {
            static char lastCommand[61] = {};
            const char *chat = reinterpret_cast<const char *>(
                reinterpret_cast<const char *>(addresses::Base.ptr) + 0x404A6C + 4ULL * 283785913);
            char lower[61];
            std::size_t i = 0;
            for (; i < 60 && chat[i]; ++i)
                lower[i] = (char)tolower((unsigned char)chat[i]);
            lower[i] = 0;
            if (std::strcmp(lower, lastCommand) == 0)
                return;
            std::strncpy(lastCommand, lower, 60);

            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                if (!addresses::Humans[h].isActive.b1)
                    continue;
                // first active human = local in practice mode
                const bool arm = std::strcmp(lower, "break arm") == 0;
                const bool leg = std::strcmp(lower, "break leg") == 0;
                if (arm || leg)
                {
                    const int side = rand() % 2;
                    BreakLimb(h, arm ? 2 + side : 4 + side);
                }
                return;
            }
        }


    }

    void Update()
    {
        CheckChatCommands();
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

            // Respawn detection: a big upward jump on any limb (0 -> 100)
            // means the human was refilled -- broken bones never carry over.
            for (int i = 0; i < 6; ++i)
            {
                if (*limbRefs[h][i].current - limbRefs[h][i].last >= 50)
                {
                    for (int j = 0; j < 6; ++j)
                        brokenLimbs[h][j] = false;
                    break;
                }
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
                    BreakBoneDrives(i);
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
