#include "NativeBloodEvents.hpp"

#include "Addresses.hpp"
#include "Diagnostics.hpp"
#include "api/BloodMarks.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace nativeblood
{
    namespace
    {
        constexpr int eventCapacity = 65536;
        constexpr int eventStride = 0x80;

        bool NearHuman(const structs::CVector3 &position)
        {
            if (!addresses::Humans.ptr) return false;
            for (std::size_t i = 0; i < structs::Human::VanillaCount; ++i)
            {
                const auto &human = addresses::Humans[i];
                if (!human.isActive.b1) continue;
                const auto &body = human.position;
                const float dx = position.x - body.x;
                const float dz = position.z - body.z;
                const float dy = position.y - body.y;
                if (dx * dx + dz * dz < 1.6f * 1.6f &&
                    dy > -0.5f && dy < 2.5f)
                    return true;
            }
            return false;
        }

        void GroundBelow(const structs::CVector3 &position)
        {
            if (!addresses::LineIntersectLevelFunc.ptr ||
                !addresses::LineIntersectResult.ptr)
                return;
            structs::CVector3 from(position.x, position.y + 0.3f, position.z);
            structs::CVector3 to(position.x, position.y - 3.0f, position.z);
            const structs::LineIntersectResult saved = *addresses::LineIntersectResult.ptr;
            const bool hit = addresses::LineIntersectLevelFunc(&from, &to, 1) != 0;
            const structs::LineIntersectResult result = *addresses::LineIntersectResult.ptr;
            *addresses::LineIntersectResult.ptr = saved;
            if (hit)
                api::bloodmarks::Add(result.position.x, result.position.y,
                                     result.position.z, result.normal.y);
        }
    }

    void Poll()
    {
#if _WIN32
        static bool initialized = false;
        static unsigned int previous = 0;
        if (!addresses::IsInGame.ptr || !*addresses::IsInGame ||
            !addresses::EventRing.ptr || !addresses::EventCount.ptr)
        {
            initialized = false;
            return;
        }
        const unsigned int current = static_cast<unsigned int>(*addresses::EventCount) & 0xffffu;
        if (!initialized)
        {
            previous = current;
            initialized = true;
            return;
        }
        const unsigned int pending = (current - previous) & 0xffffu;
        if (pending > 512)
        {
            previous = current;
            return;
        }
        for (unsigned int step = 0; step < pending; ++step)
        {
            const unsigned int index = (previous + step) % eventCapacity;
            const std::uint8_t *event = addresses::EventRing.ptr + index * eventStride;
            int type = 0, hitType = 0;
            structs::CVector3 position;
            std::memcpy(&type, event, sizeof(type));
            if (type != 1) continue;
            std::memcpy(&position, event + 0x08, sizeof(position));
            std::memcpy(&hitType, event + 0x24, sizeof(hitType));
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z))
                continue;
            const bool nearHuman = NearHuman(position);
            // Type 0 is a level impact. It can land close to a body just
            // after a body shot, but it must not create another blood mark.
            if (hitType == 3 || (hitType == 1 && nearHuman))
            {
                GroundBelow(position);
            }
            diag::Log("blood", "native hit event %u type=%d nearHuman=%d pos=%.2f %.2f %.2f",
                      index, hitType, nearHuman ? 1 : 0,
                      position.x, position.y, position.z);
        }
        previous = current;
#endif
    }
}
