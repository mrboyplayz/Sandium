#include "ItemBillboard.hpp"

#include "GLUniforms.hpp"
#include "Text.hpp"

#include "../Addresses.hpp"

#include <cmath>
#include <string>

#if _WIN32
#include <Windows.h>
#undef DrawText
#endif

namespace api
{
    namespace
    {
        constexpr float MAX_DISTANCE = 4.0f;      // world units
        constexpr float MAX_CROSSHAIR_SCORE = 0.20f; // squared NDC distance from center (~26 deg)
        // Sub Rosa's UI is drawn in a 1024x576 space scaled to the window.
        constexpr float UI_WIDTH = 1024.0f;
        constexpr float UI_HEIGHT = 576.0f;

        // clip = matrix * point, honoring the shader's transpose flag
        bool ProjectPoint(const float *m, bool transposed, const float point[3], float ndcOut[2])
        {
            float clip[4];
            for (int row = 0; row < 4; ++row)
            {
                clip[row] = transposed
                    ? m[row * 4 + 0] * point[0] + m[row * 4 + 1] * point[1] + m[row * 4 + 2] * point[2] + m[row * 4 + 3]
                    : m[0 * 4 + row] * point[0] + m[1 * 4 + row] * point[1] + m[2 * 4 + row] * point[2] + m[3 * 4 + row];
            }
            if (clip[3] < 0.05f)
                return false;
            ndcOut[0] = clip[0] / clip[3];
            ndcOut[1] = clip[1] / clip[3];
            return true;
        }
    }

    void DrawItemBillboards()
    {
        // periodic ground-truth dump for diagnosing detection trouble
        static unsigned int dumpFrames = 0;
        if (++dumpFrames % 60 == 0)
            glcap::DumpDiagnostics();

        if (!glcap::HasViewProjection())
            return;
        const float *matrix = glcap::ViewProjectionMatrix();
        const bool transposed = glcap::MatrixTransposed();
        const float *camera = glcap::ViewPosition();

        const structs::Item *bestItem = nullptr;
        float bestDistance = 1e9f;
        const float crosshair[2] = {UI_WIDTH * 0.5f, UI_HEIGHT * 0.5f};

        // mark items carried or holstered by a human (inventory incl. hands)
        bool carried[structs::Item::VanillaCount] = {};
        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
        {
            const structs::Human &human = addresses::Humans[h];
            if (!human.isActive.b1)
                continue;
            for (const structs::InventorySlot &slot : human.inventorySlots)
            {
                // ignore numberOfPlaces: its exact semantics are unclear and relying
                // on it let carried items slip through; the ID range check is the
                // real guard (sentinels are negative or out of range)
                for (const int id : {slot.firstPlaceItemID, slot.secondPlaceItemID})
                    if (id >= 0 && static_cast<std::size_t>(id) < structs::Item::VanillaCount)
                        carried[id] = true;
            }
        }

        for (std::size_t i = 0; i < structs::Item::VanillaCount; ++i)
        {
            const structs::Item &item = addresses::Items[i];
            if (!item.isActive.b1)
                continue;
            if (carried[i])
                continue; // held or holstered on a human

            const float dx = item.position.x - camera[0];
            const float dy = item.position.y - camera[1];
            const float dz = item.position.z - camera[2];
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared > MAX_DISTANCE * MAX_DISTANCE)
                continue;

            float centerNdc[2], headNdc[2];
            const float center[3] = {item.position.x, item.position.y, item.position.z};
            const float head[3] = {item.position.x, item.position.y + 0.5f, item.position.z};
            if (!ProjectPoint(matrix, transposed, center, centerNdc))
                continue;
            if (!ProjectPoint(matrix, transposed, head, headNdc))
                continue;

            const float x = (centerNdc[0] * 0.5f + 0.5f) * UI_WIDTH;
            const float y = (1.0f - (centerNdc[1] * 0.5f + 0.5f)) * UI_HEIGHT;
            // on-screen half-height of the item, in UI units
            const float halfHeight = std::fabs((1.0f - (headNdc[1] * 0.5f + 0.5f)) * UI_HEIGHT - y);

            const float crosshairDx = x - crosshair[0];
            const float crosshairDy = y - crosshair[1];
            // the crosshair must land on the item itself (with some slack),
            // so a far item whose center happens to align cannot steal the prompt
            if (std::sqrt(crosshairDx * crosshairDx + crosshairDy * crosshairDy) > halfHeight * 2.0f + 24.0f)
                continue;

            const float distance = std::sqrt(distanceSquared);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestItem = &item;
            }
        }

        if (!bestItem)
        {
            static unsigned int emptyFrames = 0;
            if (++emptyFrames % 120 == 0)
            {
                if (std::FILE *status = std::fopen("sandium_billboard.txt", "w"))
                {
                    std::fprintf(status, "no candidate. camera=(%.1f,%.1f,%.1f)\n",
                                 camera[0], camera[1], camera[2]);
                    std::fclose(status);
                }
            }
            return;
        }
        static unsigned int hitFrames = 0;
        if (++hitFrames % 60 == 0)
        {
            if (std::FILE *status = std::fopen("sandium_billboard.txt", "w"))
            {
                std::fprintf(status, "item typeID=%d dist=%.2f camera=(%.1f,%.1f,%.1f) itemPos=(%.2f,%.2f,%.2f)\n",
                             bestItem->typeID, bestDistance, camera[0], camera[1], camera[2],
                             bestItem->position.x, bestItem->position.y, bestItem->position.z);
                std::fclose(status);
            }
        }

        // the prompt itself floats a little above the item (Y is up in Sub Rosa)
        const float above[3] = {bestItem->position.x, bestItem->position.y + 0.5f, bestItem->position.z};
        float promptNdc[2];
        if (!ProjectPoint(matrix, transposed, above, promptNdc))
            return;
        const float x = (promptNdc[0] * 0.5f + 0.5f) * UI_WIDTH;
        const float y = (1.0f - (promptNdc[1] * 0.5f + 0.5f)) * UI_HEIGHT;
        std::string weaponName = "item";
        if (const structs::ItemType *type = bestItem->GetType())
            weaponName = type->GetName();
        DrawText("[E] " + weaponName, x, y, 20.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), TextAlignment::Center, true);
    }
}
