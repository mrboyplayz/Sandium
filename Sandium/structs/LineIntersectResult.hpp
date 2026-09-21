#pragma once

#include <cstddef>
#include <cstdint>

#include "Common.hpp"

namespace structs
{
#pragma pack(push, 1)
    struct LineIntersectResult
    {
        CVector3 position;       // 0x00
        CVector3 normal;         // 0x0c
        float fraction;          // 0x18
        std::uint8_t unknown1[0x54 - 0x1c];
        int areaID;              // 0x54; -1 for terrain / loose city objects
        int blockX;              // 0x58
        int blockY;              // 0x5c
        int blockZ;              // 0x60
        int materialID;          // 0x64; populated by some collision paths
        int materialScratch;     // 0x68
        int faceMaterialSlot;    // 0x6c; used with the block coordinates
    };
#pragma pack(pop)

    static_assert(offsetof(LineIntersectResult, areaID) == 0x54, "line result layout changed");
    static_assert(offsetof(LineIntersectResult, faceMaterialSlot) == 0x6c, "line result layout changed");
}
