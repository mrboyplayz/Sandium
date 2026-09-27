#include "ShadowNPC.hpp"

#if _WIN32

#include "Addresses.hpp"
#include "Flags.hpp"
#include "ItemModels.hpp"
#include "NetRedirect.hpp"
#include "api/Logging.hpp"

#include <glad/glad.h>
#include <subhook.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shadownpc
{
    namespace
    {
        using QueueFn = std::int64_t (*)(unsigned int, unsigned int *, unsigned int,
                                        unsigned int *, unsigned int *);
        QueueFn queue = nullptr;
        subhook::Hook *queueHook = nullptr;
        int blackTexture = -1;
        constexpr unsigned int ShadowRenderFlag = 0x10000000u;

        template<class T> T *At(std::uintptr_t rva)
        {
            return reinterpret_cast<T *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + rva);
        }

        std::vector<unsigned int> ShadowIDs()
        {
            const std::string value = flags::Get("shadow_humans");
            static std::string cachedValue;
            static std::vector<unsigned int> cachedIDs;
            if (value == cachedValue) return cachedIDs;
            cachedValue = value;
            cachedIDs.clear();
            std::size_t start = 0;
            while (start < value.size())
            {
                const std::size_t end = value.find(',', start);
                const std::string part = value.substr(start, end - start);
                try
                {
                    const unsigned long id = std::stoul(part);
                    if (id < structs::Human::VanillaCount)
                        cachedIDs.push_back(static_cast<unsigned int>(id));
                }
                catch (...) {}
                if (end == std::string::npos) break;
                start = end + 1;
            }
            return cachedIDs;
        }

        bool NearHuman(const unsigned int *position, const structs::Human &human)
        {
            if (!position) return false;
            const float *point = reinterpret_cast<const float *>(position);
            const auto &origin = human.position;
            const float dx = point[0] - origin.x;
            const float dy = point[1] - origin.y;
            const float dz = point[2] - origin.z;
            return std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz)
                && dx * dx + dz * dz < 1.5625f && std::abs(dy) < 2.5f;
        }

        bool IsShadowPart(unsigned int model, const unsigned int *position,
                          const std::vector<unsigned int> &ids)
        {
            const auto *body = At<int>(0x43EBBBC4);
            const auto *heads = At<int>(0x43EBC244);
            const auto *hairs = At<int>(0x43EBC2C4);
            for (unsigned int id : ids)
            {
                const auto &human = addresses::Humans[id];
                if (!human.isActive) continue;
                // The native body draw is queued at the origin and moved later
                // in its render entry. Its resource is unique to this human.
                if (body[id] == static_cast<int>(model)) return true;
                if (!NearHuman(position, human)) continue;
                if (human.genderID < 0 || human.genderID > 1) continue;
                const int offset = human.genderID * 16;
                if (human.headModelID >= 0 && human.headModelID < 16 &&
                    heads[offset + human.headModelID] == static_cast<int>(model)) return true;
                if (human.hairModelID >= 0 && human.hairModelID < 16 &&
                    hairs[offset + human.hairModelID] == static_cast<int>(model)) return true;
            }
            return false;
        }

        bool EnsureTexture()
        {
            if (blackTexture >= 0) return true;
            if (!addresses::ModelResourceCount.ptr || !addresses::ModelTextureResources.ptr ||
                !addresses::LoadTextureFunc.ptr) return false;
            const char *path = "sandium/models/shadow/black.png";
            if (!std::filesystem::is_regular_file(path))
            {
                path = "sandium/cache/shadow_black_v2.png";
                if (!std::filesystem::is_regular_file(path)) return false;
            }
            const int slot = *addresses::ModelResourceCount;
            if (slot < 0 || slot >= itemmodels::TextureMappedModelLimit) return false;
            const int texture = addresses::ModelTextureResources[slot];
            if (texture < 0) return false;
            if (!addresses::LoadTextureFunc(texture, path, 1, GL_REPEAT, GL_REPEAT,
                                            GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, 0)) return false;
            *addresses::ModelResourceCount = slot + 1;
            blackTexture = texture;
            api::GetSandiumLogger()->Log("Shadow NPC black texture loaded into resource {}", texture);
            return true;
        }

        std::int64_t QueueModel(unsigned int model, unsigned int *textures, unsigned int flags,
                                unsigned int *position, unsigned int *orientation)
        {
            subhook::ScopedHookRemove remove(queueHook);
            if (!netredirect::IsNoxusGame() || !textures || !position)
                return queue(model, textures, flags, position, orientation);
            const auto ids = ShadowIDs();
            if (ids.empty() || !IsShadowPart(model, position, ids) || !EnsureTexture())
                return queue(model, textures, flags, position, orientation);
            std::array<unsigned int, 4> replacement{
                static_cast<unsigned int>(blackTexture), static_cast<unsigned int>(blackTexture),
                static_cast<unsigned int>(blackTexture), static_cast<unsigned int>(blackTexture)};
            return queue(model, replacement.data(), flags | ShadowRenderFlag,
                         position, orientation);
        }
    }

    void Install()
    {
        queue = reinterpret_cast<QueueFn>(At<void>(0x92140));
        queueHook = new subhook::Hook(reinterpret_cast<void *>(queue),
                                      reinterpret_cast<void *>(&QueueModel),
                                      subhook::HookFlag64BitOffset);
        queueHook->Install();
    }
}

#else

namespace shadownpc { void Install() {} }

#endif
