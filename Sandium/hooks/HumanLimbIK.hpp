#pragma once

#include <cstdint>
#include <subhook.h>

#include "../structs/Common.hpp"

extern subhook::Hook *humanLimbIKHook;

std::int64_t HumanLimbIKHookFunc(
    int humanID, int trunkBoneID, int branchBoneID,
    structs::CVector3 *destination, float *destinationAxis,
    structs::CVector3 *vectorA, float reach, std::uint32_t rotationBits,
    void *quaternion, std::uint32_t *vectorB, std::uint32_t *vectorC,
    std::uint32_t *vectorD, float maximumAngle, char flags);

#if _VSCODE
#define IMPLEMENT_HOOKS 1
#endif

#if IMPLEMENT_HOOKS && _WIN32

#include <cstdio>
#include <cstring>
#include <string>

#include "../Addresses.hpp"
#include "../Flags.hpp"

subhook::Hook *humanLimbIKHook;

namespace
{
    bool m9QuaternionLocked[structs::Human::VanillaCount] = {};
    alignas(16) float m9Quaternion[structs::Human::VanillaCount][4] = {};

    bool ReadBayonetTarget(const char *name, structs::CVector3 &target)
    {
        const std::string value = flags::Get(name);
        return std::sscanf(value.c_str(), "%f %f %f",
                           &target.x, &target.y, &target.z) == 3;
    }

    bool HumanHasM9(int humanID)
    {
        if (humanID < 0 || humanID >= static_cast<int>(structs::Human::VanillaCount))
            return false;
        for (std::size_t i = 0; i < structs::Item::VanillaCount; ++i)
        {
            const auto &item = addresses::Items[i];
            if (item.isActive.b1 && item.typeID == 47 && item.parentHumanID == humanID)
                return true;
        }
        return false;
    }
}

std::int64_t HumanLimbIKHookFunc(
    int humanID, int trunkBoneID, int branchBoneID,
    structs::CVector3 *destination, float *destinationAxis,
    structs::CVector3 *vectorA, float reach, std::uint32_t rotationBits,
    void *quaternion, std::uint32_t *vectorB, std::uint32_t *vectorC,
    std::uint32_t *vectorD, float maximumAngle, char flags)
{
    subhook::ScopedHookRemove remove(humanLimbIKHook);

    const bool rightHandSolve = humanID >= 0 &&
        humanID < static_cast<int>(structs::Human::VanillaCount) &&
        trunkBoneID == 2 && branchBoneID == 7;
    const bool holdingM9 = rightHandSolve && HumanHasM9(humanID);

    if (rightHandSolve && !holdingM9)
        m9QuaternionLocked[humanID] = false;

    if (destination && holdingM9)
    {
        const bool stabbing = (addresses::Humans[humanID].inputFlags & 1u) != 0;
        structs::CVector3 fixedTarget;
        if (ReadBayonetTarget(stabbing ? "bayonet_stab" : "bayonet_hold", fixedTarget))
            *destination = fixedTarget;

        // IDA: quaternion is the final hand-bone orientation target. The caller
        // rebuilds it from camera yaw/pitch every frame. Capture the natural
        // orientation on equip and reuse it so looking around cannot steer the
        // hand. Do not touch destinationAxis: replacing that basis caused the
        // forward-left offset reported in testing.
        if (quaternion)
        {
            if (!m9QuaternionLocked[humanID])
            {
                std::memcpy(m9Quaternion[humanID], quaternion,
                            sizeof(m9Quaternion[humanID]));
                m9QuaternionLocked[humanID] = true;
            }
            else
            {
                std::memcpy(quaternion, m9Quaternion[humanID],
                            sizeof(m9Quaternion[humanID]));
            }
        }
    }

    return addresses::HumanLimbIKFunc(
        humanID, trunkBoneID, branchBoneID, destination, destinationAxis,
        vectorA, reach, rotationBits, quaternion, vectorB, vectorC, vectorD,
        maximumAngle, flags);
}

#endif
