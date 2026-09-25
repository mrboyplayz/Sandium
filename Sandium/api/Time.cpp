#include "Time.hpp"

#include "../Addresses.hpp"
#include "../Diagnostics.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if _WIN32
#include <Windows.h>
#endif

namespace api
{
    namespace
    {
        constexpr int PACKET_SLOT_MAX = 0x1FFF;
        constexpr float TAU = 6.283185307179586f;

        std::atomic<bool> scanStarted{false};
        std::atomic<bool> locked{false};
        float *foundAngle = nullptr;
        std::mutex foundMutex;
        int staleFrames = 0;

        // defined further down in this namespace
        bool RelationHolds(float angle, int slot);
        void ScanThreadProc();

        // a locked candidate must keep matching the packet relation; a stale or
        // false-positive address must never be written to
        bool LockStillValid()
        {
            if (!foundAngle)
                return false;
            const float angle = *foundAngle;
            const int slot = *reinterpret_cast<const int *>(reinterpret_cast<const char *>(foundAngle) + 152);
            if (angle > 0.001f && angle < TAU && RelationHolds(angle, slot))
            {
                staleFrames = 0;
                return true;
            }
            // tolerate brief wrap points (midnight), but give up eventually
            return ++staleFrames < 600;
        }

        void RestartScan()
        {
            locked = false;
            staleFrames = 0;
            addresses::SunAngle.ptr = nullptr;
            std::thread(ScanThreadProc).detach();
        }

#if _WIN32
        bool CommittedMemory(const void *address)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            return VirtualQuery(address, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT;
        }

        bool RegionAllowed(const MEMORY_BASIC_INFORMATION &mbi)
        {
            const std::uintptr_t allocBase = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
            if (allocBase == reinterpret_cast<std::uintptr_t>(GetModuleHandleA("Sandium.dll")))
                return false;
            if (mbi.Type == MEM_IMAGE)
                return allocBase == reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
            return mbi.Type == MEM_PRIVATE;
        }

        // packet encoding: slot == clamp((int)(angle * 3600 / (2pi)), 0, 8191)
        // (the game also writes 1 in a special night case)
        bool RelationHolds(float angle, int slot)
        {
            if (!(angle > 0.001f) || !(angle < TAU))
                return false; // rejects zeroed memory too
            float scaled = (angle * 3600.0f) * 0.15915494f;
            int expected = static_cast<int>(scaled);
            if (expected < 0)
                expected = 0;
            if (expected > PACKET_SLOT_MAX)
                expected = PACKET_SLOT_MAX;
            return slot == expected || slot == 1;
        }

        std::vector<const char *> ScanCandidates()
        {
            std::vector<const char *> candidates;
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            std::uintptr_t address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
            const std::uintptr_t maxAddress = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
            while (address < maxAddress)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<void *>(address), &mbi, sizeof(mbi)))
                    break;
                const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                const std::uintptr_t size = mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && RegionAllowed(mbi) &&
                    (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY) &&
                    size > 152 + 4)
                {
                    const char *bytes = reinterpret_cast<const char *>(base);
                    for (std::uintptr_t offset = 0; offset + 156 <= size; offset += 4)
                    {
                        const float angle = *reinterpret_cast<const float *>(bytes + offset);
                        const int slot = *reinterpret_cast<const int *>(bytes + offset + 152);
                        if (RelationHolds(angle, slot))
                            candidates.push_back(bytes + offset);
                    }
                }
                address = base + size;
            }
            return candidates;
        }

        struct Candidate
        {
            const char *address;
            float angle;
            int slot;
        };

        void ScanThreadProc()
        {
            std::vector<Candidate> previous;
            int confirmed = 0;
            for (int attempt = 0; attempt < 600 && !locked; ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (!addresses::SunAngle.ptr)
                    continue;

                std::vector<Candidate> current;
                for (const char *candidate : ScanCandidates())
                    current.push_back({candidate,
                                       *reinterpret_cast<const float *>(candidate),
                                       *reinterpret_cast<const int *>(candidate + 152)});

                if (previous.empty())
                {
                    previous = current;
                    continue;
                }

                // keep candidates seen before whose value changed between scans:
                // the real day-cycle angle moves, static lookalikes do not
                std::vector<Candidate> survivors;
                for (const Candidate &candidate : current)
                    for (const Candidate &prev : previous)
                        if (prev.address == candidate.address &&
                            (prev.angle != candidate.angle || prev.slot != candidate.slot))
                            survivors.push_back(candidate);

                if (survivors.empty())
                {
                    confirmed = 0;
                    previous = current;
                    continue;
                }

                ++confirmed;
                previous = survivors;
                if (confirmed >= 2)
                {
                    const std::lock_guard<std::mutex> lock(foundMutex);
                    foundAngle = reinterpret_cast<float *>(const_cast<char *>(survivors.front().address));
                    addresses::SunAngle.ptr = foundAngle;
                    locked = true;
                    diag::Log("time", "locked angle at %p (value %.4f)",
                              reinterpret_cast<void *>(foundAngle),
                              *foundAngle);
                    return;
                }
            }
        }
#endif
    }

    bool Time::IsValid()
    {
#if _WIN32
        if (!scanStarted.exchange(true))
            std::thread(ScanThreadProc).detach();
        if (!addresses::SunAngle.ptr)
            return false;
        if (!CommittedMemory(addresses::SunAngle.ptr))
            return false;
        if (locked && !LockStillValid())
            RestartScan();
        return addresses::SunAngle.ptr != nullptr;
#else
        return false;
#endif
    }

    float Time::GetSunAngle()
    {
#if _WIN32
        if (!Time::IsValid())
            return 0.0f;
        return *addresses::SunAngle.ptr;
#else
        return 0.0f;
#endif
    }

    void Time::SetSunAngle(float angle)
    {
#if _WIN32
        if (!Time::IsValid())
            return;
        angle = std::fmod(angle, TAU);
        if (angle < 0.0f)
            angle += TAU;
        *addresses::SunAngle.ptr = angle;
#else
        (void)angle;
#endif
    }

    unsigned int Time::GetSunTime()
    {
#if _WIN32
        if (!addresses::SunTime.ptr || !CommittedMemory(addresses::SunTime.ptr))
            return 0;
        return *addresses::SunTime.ptr;
#else
        return 0;
#endif
    }

    void Time::SetSunTime(unsigned int sunTime)
    {
#if _WIN32
        if (!addresses::SunTime.ptr || !CommittedMemory(addresses::SunTime.ptr))
            return;
        *addresses::SunTime.ptr = sunTime & 0x3FFFFFFFu;
#endif
    }

    void Time::DumpDebug()
    {
#if _WIN32
        if (!addresses::SunAngle.ptr)
            return;
        const char *base = reinterpret_cast<const char *>(addresses::SunAngle.ptr);
        const auto f = [&](int offset) { return *reinterpret_cast<const float *>(base + offset); };
        const auto i = [&](int offset) { return *reinterpret_cast<const int *>(base + offset); };
        diag::Log("time",
            "angle=%.4f speedA(+4)=%.4f speedB(+8)=%.4f dampAcc(+12)=%.4f "
            "divisor(+24)=%.4f dragK(+28)=%.4f divisor2(+32)=%.4f "
            "weather(+48)=%.4f smooth(+56)=%.4f packetSlot(+152)=%d",
            f(0), f(4), f(8), f(12), f(24), f(28), f(32), f(48), f(56), i(152));
#endif
    }
}
