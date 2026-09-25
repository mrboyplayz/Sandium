#include "Playermodel.hpp"
#include "Addresses.hpp"
#include "ItemModels.hpp"
#include "api/Logging.hpp"
#include <glad/glad.h>
#include <subhook.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace playermodel
{
    namespace
    {
        constexpr int InternHead = 5, InternHair = 15, InternBody = 3;
        using SkinFn = std::int64_t (*)(unsigned int, int);
        using QueueFn = std::int64_t (*)(unsigned int, unsigned int *, unsigned int,
                                        unsigned int *, unsigned int *);
        using LoadCharacterFn = std::int64_t (*)(int, const char *);
        SkinFn skin = nullptr;
        QueueFn queue = nullptr;
        subhook::Hook *skinHook = nullptr, *queueHook = nullptr;
        bool attempted = false, ready = false;
        int enabled = 0, texture = -1, headResource = -1;
        std::array<std::array<float, 3>, 16> exportedBinds{};
        std::array<unsigned char, 8192> customBodyResources{};

        template<class T> T *At(std::uintptr_t rva)
        {
            return reinterpret_cast<T *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + rva);
        }

        void ApplySelection()
        {
            if (enabled && ready)
            {
                *At<int>(0x11E461EC) = InternHead;
                *At<int>(0x11E461F8) = InternHair;
            }
            else if (*At<int>(0x11E461EC) == InternHead)
            {
                *At<int>(0x11E461EC) = 0;
                *At<int>(0x11E461F8) = 0;
            }
        }

        std::int64_t SkinHuman(unsigned int humanID, int resource)
        {
            subhook::ScopedHookRemove remove(skinHook);
            if (!ready || humanID >= structs::Human::VanillaCount ||
                addresses::Humans[humanID].headModelID != InternHead)
                return skin(humanID, resource);
            auto &human = addresses::Humans[humanID];
            const int model = human.modelID, head = human.headModelID,
                      hair = human.hairModelID,
                      tie = human.tieColorID, necklace = human.necklaceID;
            human.modelID = InternBody;
            human.headModelID = InternHead;
            human.hairModelID = InternHair;
            human.tieColorID = human.necklaceID = 0;
            const auto result = skin(humanID, resource);
            human.modelID = model;
            human.headModelID = head;
            human.hairModelID = hair;
            human.tieColorID = tie;
            human.necklaceID = necklace;
            return result;
        }

        // The skin pass uses transform feedback, not a texture sampler.
        // Substitute texture resources only when the native renderer queues it.
        std::int64_t QueueModel(unsigned int model, unsigned int *textures, unsigned int flags,
                                unsigned int *position, unsigned int *orientation)
        {
            subhook::ScopedHookRemove remove(queueHook);
            const bool custom = ready &&
                (model == static_cast<unsigned int>(headResource) ||
                 (model < customBodyResources.size() && customBodyResources[model]));
            if (!custom) return queue(model, textures, flags, position, orientation);
            std::array<unsigned int, 4> replacement;
            std::memcpy(replacement.data(), textures, sizeof(replacement));
            replacement[0] = replacement[1] = texture;
            return queue(model, replacement.data(), flags, position, orientation);
        }

        template<class T> T Read(std::ifstream &file)
        {
            T value{};
            if (!file.read(reinterpret_cast<char *>(&value), sizeof(value)))
                throw std::runtime_error("Truncated Intern CMC");
            return value;
        }

        struct ExportVertex { std::array<float, 3> position; std::array<float, 2> uv; };

        std::vector<ExportVertex> ReadExpandedUVs()
        {
            std::ifstream file("data/model/intern_body.cmc", std::ios::binary);
            if (Read<unsigned int>(file) != 0x646F4D43 || Read<unsigned int>(file) != 2 ||
                Read<unsigned int>(file) != 16) throw std::runtime_error("Invalid Intern CMC header");
            file.seekg(16 * 12, std::ios::cur);
            const auto count = Read<unsigned int>(file);
            if (!count || count > 8192) throw std::runtime_error("Invalid Intern vertex count");
            std::vector<ExportVertex> vertices(count);
            for (auto &vertex : vertices)
            {
                vertex.position = Read<std::array<float, 3>>(file);
                file.seekg(256, std::ios::cur);
                auto &uv = vertex.uv;
                uv = Read<std::array<float, 2>>(file);
                if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]) ||
                    uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1)
                    throw std::runtime_error("Invalid Intern UV");
            }
            const auto faces = Read<unsigned int>(file);
            if (!faces || faces > 8192 / 3) throw std::runtime_error("Intern exceeds native CMC budget");
            std::vector<ExportVertex> result;
            result.reserve(faces * 3);
            for (unsigned int i = 0; i < faces * 3; ++i)
            {
                const auto index = Read<unsigned int>(file);
                if (index >= count) throw std::runtime_error("Invalid Intern triangle index");
                result.push_back(vertices[index]);
            }
            return result;
        }

        void RestoreUVs(int slot, const std::vector<ExportVertex> &uvs)
        {
            auto *header = At<int>(0x269A49C0 + std::uintptr_t(slot) * 786828);
            if (header[0] != static_cast<int>(uvs.size()) || header[1] != 16)
                throw std::runtime_error("Native CMC upload does not match exported mesh");
            auto *vertices = reinterpret_cast<float *>(header + 2);
            // The native CMC loader assembles shoulder vertices around its
            // scratch humanoid (arms can be centred at 0,.164,0). Imported
            // meshes use a common bind space instead. Normalize this custom
            // slot's input positions AND inverse-bind translations together;
            // animation, transform feedback and drawing remain native.
            auto *binds = At<float>(0x26A649CC + std::uintptr_t(slot) * 786828);
            for (std::size_t i = 0; i < exportedBinds.size(); ++i)
                std::memcpy(binds + i * 6, exportedBinds[i].data(), 3 * sizeof(float));
            for (std::size_t i = 0; i < uvs.size(); ++i)
            {
                for (int axis = 0; axis < 3; ++axis)
                    vertices[i * 16 + axis] = uvs[i].position[axis];
                vertices[i * 16 + 3] = uvs[i].uv[0];
                vertices[i * 16 + 4] = uvs[i].uv[1];
            }
            // Native files use clockwise winding; edge C x edge B is outward.
            for (std::size_t i = 0; i < uvs.size(); i += 3)
            {
                const auto &a = uvs[i].position, &b = uvs[i + 1].position, &c = uvs[i + 2].position;
                const std::array<float, 3> u{c[0]-a[0], c[1]-a[1], c[2]-a[2]};
                const std::array<float, 3> v{b[0]-a[0], b[1]-a[1], b[2]-a[2]};
                std::array<float, 3> normal{u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
                const float length = std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
                if (length > 0) for (auto &component : normal) component /= length;
                for (int corner = 0; corner < 3; ++corner)
                    std::memcpy(vertices + (i + corner)*16 + 5, normal.data(), 3*sizeof(float));
            }
            const int bufferID = At<int>(0x43E987C4)[slot];
            if (bufferID < 0 || bufferID >= 8192) throw std::runtime_error("Invalid native CMC buffer ID");
            const GLuint buffer = At<GLuint>(0x43E9B0C0)[bufferID * 2];
            if (!glIsBuffer(buffer)) throw std::runtime_error("Native CMC buffer missing");
            GLint previous = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBufferData(GL_ARRAY_BUFFER, uvs.size() * 16 * sizeof(float), vertices, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, previous);
        }
    }

    void Install()
    {
#if _WIN32
        skin = reinterpret_cast<SkinFn>(At<void>(0x22DF0));
        queue = reinterpret_cast<QueueFn>(At<void>(0x92140));
        skinHook = new subhook::Hook(reinterpret_cast<void *>(skin), reinterpret_cast<void *>(&SkinHuman),
                                     subhook::HookFlag64BitOffset);
        queueHook = new subhook::Hook(reinterpret_cast<void *>(queue), reinterpret_cast<void *>(&QueueModel),
                                      subhook::HookFlag64BitOffset);
        skinHook->Install();
        queueHook->Install();
#endif
    }
    void InitializeAssets()
    {
#if _WIN32
        if (attempted) return;
        attempted = true;
        // Retire the old broken preset rather than silently applying it again.
        ApplySelection();
        try
        {
            const auto uvs = ReadExpandedUVs();
            std::ifstream bindFile("sandium/models/intern/intern_bind.bin", std::ios::binary);
            exportedBinds = Read<decltype(exportedBinds)>(bindFile);
            for (const auto &bind : exportedBinds)
                for (float value : bind)
                    if (!std::isfinite(value) || std::abs(value) > 4.0f)
                        throw std::runtime_error("Invalid Intern bind translation");
            for (const char *name : {"data/model/intern_head.cmo", "data/model/intern_empty_hair.cmo",
                                    "sandium/models/intern/intern.png"})
                if (!std::filesystem::is_regular_file(name)) throw std::runtime_error(name);
            const int first = *addresses::ModelResourceCount;
            if (first < 0 || first + 2 > itemmodels::TextureMappedModelLimit)
                throw std::runtime_error("Native model resource pool full");
            *addresses::ModelResourceCount += 2;
            headResource = first;
            const int hairResource = first + 1;
            texture = addresses::ModelTextureResources[headResource];
            if (!addresses::LoadTextureFunc(texture, "sandium/models/intern/intern.png", 1,
                                            GL_REPEAT, GL_REPEAT, GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, 0))
                throw std::runtime_error("Native texture loader rejected Intern PNG");
            // Load outside vanilla head resource ranges so the CMO uploader
            // does not rewrite UVs into its skin/hair/eye palette selectors.
            if (!addresses::LoadCMOFunc(headResource, -1, "intern_head") ||
                !addresses::LoadCMOFunc(hairResource, -1, "intern_empty_hair"))
                throw std::runtime_error("Native CMO loader rejected Intern assets");
            const auto load = reinterpret_cast<LoadCharacterFn>(At<void>(0xE8580));
            for (int slot : {InternBody, 8 + InternBody})
            {
                load(slot, "intern_body");
                RestoreUVs(slot, uvs);
            }
            for (int offset : {0, 16})
            {
                At<int>(0x43EBC244)[offset + InternHead] = headResource;
                At<int>(0x43EBC2C4)[offset + InternHair] = hairResource;
            }
            ready = true;
            api::GetSandiumLogger()->Log("Intern rebuilt: native body vertices {}, texture resource {}, head resource {}",
                                         uvs.size(), texture, headResource);
        }
        catch (const std::exception &error)
        {
            api::GetSandiumLogger()->Log("<red>Intern disabled: {}", error.what());
        }
#endif
    }

    void RefreshRenderResourceCache()
    {
#if _WIN32
        if (!ready) return;
        customBodyResources.fill(0);
        const auto *resources = At<unsigned int>(0x43EBBBC4);
        for (unsigned int i = 0; i < structs::Human::VanillaCount; ++i)
        {
            const unsigned int model = resources[i];
            if (addresses::Humans[i].headModelID == InternHead &&
                model > 0 && model < customBodyResources.size())
                customBodyResources[model] = 1;
        }
#endif
    }
    void DrawAppearanceOption()
    {
#if _WIN32
        // Intern preset retired from the menu. The native pipeline below is
        // kept intact for the next playermodel; re-add a toggle to re-enable.
        (void)enabled;
        if (!ready || *addresses::IsInGame || *addresses::MenuTypeID != 5) return;
        return;
#endif
    }
    void AfterMenuDraw()
    {
#if _WIN32
        if (enabled) ApplySelection();
#endif
    }
}
