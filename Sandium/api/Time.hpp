#pragma once

namespace api
{
    struct Time
    {
        // Sun angle of the day/night cycle, radians in [0, 2pi). Reads return 0
        // and writes do nothing until the address probe has validated itself.
        static float GetSunAngle();
        static void SetSunAngle(float angle);
        static bool IsValid();
        static void DumpDebug();
        static unsigned int GetSunTime();
        static void SetSunTime(unsigned int sunTime);
    };
}
