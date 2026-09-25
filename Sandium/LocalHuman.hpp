#pragma once

#include <cstddef>
#include <utility>

#include "Addresses.hpp"
#include "api/GLUniforms.hpp"

namespace localhuman
{
    inline std::pair<structs::Human *, int> Find()
    {
        const float *camera = api::glcap::HasViewPosition()
            ? api::glcap::ViewPosition()
            : (api::glcap::LastViewPositionValid()
                ? api::glcap::LastViewPosition() : nullptr);
        structs::Human *first = nullptr;
        int firstID = -1;
        structs::Human *closest = nullptr;
        int closestID = -1;
        float closestDistance = 400.0f; // camera is normally within 20 units of its body
        for (std::size_t i = 0; i < structs::Human::VanillaCount; ++i)
        {
            auto &human = addresses::Humans[i];
            if (!human.isActive.b1) continue;
            if (!first) { first = &human; firstID = static_cast<int>(i); }
            if (!camera) continue;
            const float dx = human.position.x - camera[0];
            const float dy = human.position.y - camera[1];
            const float dz = human.position.z - camera[2];
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closest = &human;
                closestID = static_cast<int>(i);
            }
        }
        return closest ? std::make_pair(closest, closestID)
                       : std::make_pair(first, firstID);
    }
}
