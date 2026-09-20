#include "CustomModels.hpp"

#include "Addresses.hpp"
#include "GeneratedBoomboxModel.hpp"
#include "api/GLUniforms.hpp"
#include "api/Image.hpp"
#include "api/Logging.hpp"
#include "hooks/CreateItem.hpp"
#include "TypeManager.hpp"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <subhook.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace custommodels
{
    namespace
    {
        std::shared_ptr<api::Image> texture;
        GLuint vao = 0, vbo = 0, program = 0;
        GLint vpLocation = -1, modelLocation = -1, textureLocation = -1;
        int boomboxType = -1;
        int boomboxBaseType = -1;
        bool initialized = false;
        bool initFailed = false;
        bool spawned = false;
        bool wasInGame = false;
        bool reportedDraw = false;
        std::string boomboxTypeID = "suitium:boombox";
        std::array<bool, structs::Item::VanillaCount> boomboxItems{};

        constexpr int REVOLVER_TYPE = 46; // first genuinely expanded item slot
        constexpr int MAGNUM_TYPE = 5;
        bool revolverConfigured = false;
        bool revolverModelLoaded = false;
        bool revolverSpawned = false;
        bool revolverWasInGame = false;
        std::string revolverTypeID = "sandium:revolver";

        GLuint Compile(GLenum type, const char *source)
        {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint okay = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
            if (!okay) { glDeleteShader(shader); return 0; }
            return shader;
        }

        bool Initialize()
        {
            if (initialized) return true;
            if (initFailed || !api::glcap::HasViewProjection()) return false;
            try { texture = api::Image::Load("sandium/models/boombox/boomboxmesh.png"); }
            catch (const std::exception &error)
            {
                api::GetSandiumLogger()->Log("Boombox texture error: {}", error.what());
                initFailed = true;
                return false;
            }

            const char *vs = "#version 330 core\nlayout(location=0) in vec3 p; layout(location=1) in vec2 uv; layout(location=2) in vec3 n; uniform mat4 vp; uniform mat4 model; out vec2 tex; out float light; void main(){ vec3 normal=normalize(mat3(model)*n); light=0.32+0.68*max(dot(normal,normalize(vec3(0.3,0.8,0.5))),0.0); tex=uv; gl_Position=vp*model*vec4(p,1.0); }";
            const char *fs = "#version 330 core\nin vec2 tex; in float light; uniform sampler2D image; out vec4 color; void main(){ vec4 c=texture(image,tex); if(c.a<0.05) discard; color=vec4(c.rgb*light,c.a); }";
            GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
            if (!v || !f) { api::GetSandiumLogger()->Log("Boombox shader compilation failed"); initFailed = true; return false; }
            program = glCreateProgram(); glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
            glDeleteShader(v); glDeleteShader(f);
            GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) { api::GetSandiumLogger()->Log("Boombox shader link failed"); initFailed = true; return false; }
            vpLocation = glGetUniformLocation(program, "vp"); modelLocation = glGetUniformLocation(program, "model"); textureLocation = glGetUniformLocation(program, "image");
            GLint previousVao = 0, previousArrayBuffer = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
            glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, boomboxVertices.size() * sizeof(RuntimeModelVertex), boomboxVertices.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RuntimeModelVertex), (void *)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RuntimeModelVertex), (void *)(3 * sizeof(float)));
            glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(RuntimeModelVertex), (void *)(5 * sizeof(float)));
            glBindVertexArray(previousVao);
            glBindBuffer(GL_ARRAY_BUFFER, previousArrayBuffer);
            initialized = true;
            api::GetSandiumLogger()->Log("Boombox: uploaded {} OBJ render vertices and texture {}", boomboxVertices.size(), texture->TextureID());
            return true;
        }

        void SpawnForPlayer()
        {
            if (spawned || boomboxType < 0 || boomboxBaseType < 0) return;
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                auto &human = addresses::Humans[h];
                if (!human.isActive.b1) continue;
                int slot = -1;
                for (int s = 0; s < 6; ++s)
                    if (s != 2 && human.inventorySlots[s].numberOfPlaces == 0) { slot = s; break; }
                if (slot < 0) return;
                structs::CVector3 pos = human.position, velocity{};
                structs::COrientation orientation{};
                int id = -1;
                { subhook::ScopedHookRemove remove(createItemHook); id = addresses::CreateItemFunc(boomboxBaseType, &pos, &velocity, &orientation); }
                if (id < 0 || id >= static_cast<int>(structs::Item::VanillaCount)) return;
                auto &item = addresses::Items[id];
                // Keep the engine-facing type in the valid vanilla range. The
                // separate boomboxItems registry is the custom identity used by
                // Suitium; type 46 makes vanilla HUD/hand code index out of bounds.
                item.typeID = boomboxBaseType;
                // Use the game's attachment routine. Directly writing the parent
                // and inventory fields leaves the native render/physics object at
                // an all-zero transform and breaks later drop bookkeeping.
                if (!addresses::AttachItemFunc.ptr || addresses::AttachItemFunc(id, -1, static_cast<int>(h), slot) == 0)
                {
                    item.isActive = {};
                    api::GetSandiumLogger()->Log("Boombox error: native inventory attachment failed for item {}", id);
                    return;
                }
                boomboxItems[id] = true; spawned = true;
                api::GetSandiumLogger()->Log("Boombox: spawned item {} for human {} in inventory slot {}", id, h, slot);
                return;
            }
        }

        void DrawItem(const structs::CVector3 &p, const structs::COrientation &o)
        {
            constexpr float scale = 1.0f;
            const float model[16] = {o.right.x*scale,o.right.y*scale,o.right.z*scale,0, o.up.x*scale,o.up.y*scale,o.up.z*scale,0, o.back.x*scale,o.back.y*scale,o.back.z*scale,0, p.x,p.y,p.z,1};
            glUniformMatrix4fv(modelLocation, 1, GL_FALSE, model);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(boomboxVertices.size()));
        }
    }

    void ConfigureBoomboxType()
    {
        for (std::size_t i = 0; i < structs::ItemType::VanillaCount; ++i)
            if (std::string(addresses::ItemTypes[i].name) == "Box") boomboxBaseType = static_cast<int>(i);
        if (boomboxBaseType < 0)
        {
            api::GetSandiumLogger()->Log("Boombox error: could not find stable backing item type");
            return;
        }
        boomboxType = static_cast<int>(structs::ItemType::VanillaCount);
        auto &definition = addresses::ItemTypes[boomboxType];
        definition = addresses::ItemTypes[boomboxBaseType];
        definition.customData.index = boomboxType;
        definition.customData.typeIDPtr = &boomboxTypeID;
        definition.SetName("Boombox");
        api::GetSandiumLogger()->Log("Registered boombox tracking type with stable backing slot {}", boomboxBaseType);
    }

    void ConfigureRevolverType()
    {
        // The native table has been relocated to a 64-entry allocation, so the
        // revolver can own ID 46 without replacing any vanilla item type.
        auto *typeID = addresses::ItemTypes[REVOLVER_TYPE].customData.typeIDPtr;
        addresses::ItemTypes[REVOLVER_TYPE] = addresses::ItemTypes[MAGNUM_TYPE];
        addresses::ItemTypes[REVOLVER_TYPE].customData.index = REVOLVER_TYPE;
        addresses::ItemTypes[REVOLVER_TYPE].customData.typeIDPtr = typeID;
        *typeID = revolverTypeID;
        addresses::ItemTypes[REVOLVER_TYPE].SetName("Revolver");
        addresses::ItemTypes[REVOLVER_TYPE].ammoCount = 6;
        GetItemTypeManager()->RegisterID("sandium", "revolver", REVOLVER_TYPE);
        revolverConfigured = true;
        // The copied Magnum definition already owns valid native mesh/material
        // pointers.  Keep those for the item-table expansion test; calling the
        // CMO upload routine with ID 46 reaches a second fixed-size renderer
        // allocation which still needs to be relocated separately.
        revolverModelLoaded = true;
        api::GetSandiumLogger()->Log(
            "Registered sandium:revolver as native weapon type {} using inherited Magnum rendering",
            REVOLVER_TYPE);
    }

    void UpdateRevolver()
    {
        if (!revolverConfigured || !addresses::IsInGame.ptr)
            return;

        const bool inGame = *addresses::IsInGame;
        if (!inGame)
        {
            revolverSpawned = false;
            revolverWasInGame = false;
            return;
        }
        if (!revolverWasInGame)
        {
            revolverSpawned = false;
            revolverWasInGame = true;
        }
        if (revolverSpawned || !revolverModelLoaded)
            return;

        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
        {
            auto &human = addresses::Humans[h];
            if (!human.isActive.b1)
                continue;
            int slot = -1;
            for (int s = 0; s < 6; ++s)
                if (s != 2 && human.inventorySlots[s].numberOfPlaces == 0)
                {
                    slot = s;
                    break;
                }
            if (slot < 0)
                return;

            structs::CVector3 position = human.position;
            structs::CVector3 velocity{};
            structs::COrientation orientation{};
            int itemID = -1;
            {
                subhook::ScopedHookRemove remove(createItemHook);
                itemID = addresses::CreateItemFunc(REVOLVER_TYPE, &position, &velocity, &orientation);
            }
            if (itemID < 0 || itemID >= static_cast<int>(structs::Item::VanillaCount))
                return;
            addresses::Items[itemID].ammoCount = 6;
            if (!addresses::AttachItemFunc.ptr ||
                addresses::AttachItemFunc(itemID, -1, static_cast<int>(h), slot) == 0)
            {
                addresses::Items[itemID].isActive = {};
                api::GetSandiumLogger()->Log("Revolver error: native inventory attachment failed for item {}", itemID);
                return;
            }
            revolverSpawned = true;
            api::GetSandiumLogger()->Log("Spawned loaded revolver item {} for human {} in slot {}", itemID, h, slot);
            return;
        }
    }

    void UpdateAndDraw()
    {
        const bool inGame = addresses::IsInGame.ptr && *addresses::IsInGame;
        if (!inGame)
        {
            spawned = false; wasInGame = false;
            boomboxItems.fill(false); return;
        }
        if (!wasInGame) { spawned = false; wasInGame = true; }
        SpawnForPlayer();
        if (boomboxType < 0 || !api::glcap::HasViewProjection() || !api::glcap::HasViewPosition() || !Initialize()) return;
        GLint oldProgram = 0, oldVao = 0, oldArrayBuffer = 0, oldTexture = 0, oldActiveTexture = 0;
        GLint oldBlendSrcRgb = 0, oldBlendDstRgb = 0, oldBlendSrcAlpha = 0, oldBlendDstAlpha = 0;
        GLboolean oldDepthMask = GL_TRUE;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
        glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
        GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE);
        // This pass replaces the invisible virtual item. Its physical Box
        // backing can already have populated the depth buffer around the same
        // position, so testing against scene depth hides the boombox completely.
        // The corrected camera matrix keeps this pass bounded to the model.
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // Sub Rosa exposes combined matrices only; build a clean view from the
        // renderer's camera position and the local player's live view angles.
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        const float aspect = viewport[3] > 0 ? static_cast<float>(viewport[2]) / static_cast<float>(viewport[3]) : 16.0f / 9.0f;
        const float *camera = api::glcap::ViewPosition();
        const glm::vec3 cameraPosition(camera[0], camera[1], camera[2]);
        const structs::Human *localHuman = nullptr;
        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            if (addresses::Humans[h].isActive.b1) { localHuman = &addresses::Humans[h]; break; }
        if (!localHuman) return;
        const int localHumanID = static_cast<int>(localHuman - addresses::Humans.ptr);
        const float yaw = localHuman->viewYaw;
        const float pitch = localHuman->viewPitch;
        const glm::vec3 forward = glm::normalize(glm::vec3(std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)));
        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        const glm::vec3 cameraRight = glm::normalize(glm::cross(worldUp, forward));
        const glm::vec3 cameraUp = glm::normalize(glm::cross(forward, cameraRight));
        const glm::mat4 view = glm::lookAt(cameraPosition, cameraPosition + forward, worldUp);
        const glm::mat4 projection = glm::perspective(glm::radians(70.0f), aspect, 0.05f, 4096.0f);
        const glm::mat4 viewProjection = projection * view;
        glUseProgram(program);
        glUniformMatrix4fv(vpLocation, 1, GL_FALSE, glm::value_ptr(viewProjection));
        glUniform1i(textureLocation, 0);
        glBindTexture(GL_TEXTURE_2D, texture->TextureID()); glBindVertexArray(vao);
        for (std::size_t i = 0; i < structs::Item::VanillaCount; ++i)
        {
            if (boomboxItems[i] && !addresses::Items[i].isActive.b1) boomboxItems[i] = false;
            if (boomboxItems[i] && addresses::Items[i].isActive.b1)
            {
                const auto &item = addresses::Items[i];
                const float dx = item.position.x - camera[0];
                const float dy = item.position.y - camera[1];
                const float dz = item.position.z - camera[2];
                structs::CVector3 drawPosition = item.position;
                structs::COrientation drawOrientation = item.orientation;

                // Ghidra: FUN_1400cc610 copies the authoritative render transform
                // into the rigid-body render object at 0x449816e0 (stride 0xbc).
                // Held items are attached to the first-person hand there; Item::position
                // remains a simulation/world position and is the reason the old pass
                // floated beside the player.
                if (item.rigidBodyID >= 0 && item.rigidBodyID < 0x2000)
                {
                    const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
                    const auto renderObject = base + 0x449816e0ull + static_cast<std::uintptr_t>(item.rigidBodyID) * 0xbcull;
                    const float *position = reinterpret_cast<const float *>(renderObject + 0x18);
                    const float *right = reinterpret_cast<const float *>(renderObject + 0x3c);
                    const float *up = reinterpret_cast<const float *>(renderObject + 0x48);
                    const float *back = reinterpret_cast<const float *>(renderObject + 0x54);
                    drawPosition = structs::CVector3(position[0], position[1], position[2]);
                    drawOrientation.right = structs::CVector3(right[0], right[1], right[2]);
                    drawOrientation.up = structs::CVector3(up[0], up[1], up[2]);
                    drawOrientation.back = structs::CVector3(back[0], back[1], back[2]);
                }
                if (item.parentHumanID >= 0)
                {
                    // Noxus keeps the storage-slot ID even while its separate
                    // first-person viewmodel is active, and its slot-0 references
                    // are not reliable. The native render object itself already
                    // contains the correct visible/attached transform.
                    if (item.parentHumanID != localHumanID)
                        continue;
                    // First-person held models use a camera-space pass; the
                    // world rigid-body matrix is intentionally invalid there.
                    // Use a camera-relative transform for that pass and the real
                    // item/rigid-body transform after it is dropped.
                    const glm::vec3 held = cameraPosition + forward * 0.72f + cameraRight * 0.30f - cameraUp * 0.25f;
                    drawPosition = structs::CVector3(held.x, held.y, held.z);
                    drawOrientation.right = structs::CVector3(cameraRight.x, cameraRight.y, cameraRight.z);
                    drawOrientation.up = structs::CVector3(cameraUp.x, cameraUp.y, cameraUp.z);
                    drawOrientation.back = structs::CVector3(forward.x, forward.y, forward.z);
                    glDisable(GL_DEPTH_TEST);
                }
                else
                {
                    // A dropped boombox follows its real physics transform and
                    // participates in ordinary world occlusion.
                    if (dx * dx + dy * dy + dz * dz < 0.04f) continue;
                    glEnable(GL_DEPTH_TEST);
                }
                DrawItem(drawPosition, drawOrientation);
                if (!reportedDraw)
                {
                    const glm::vec4 clip = viewProjection * glm::vec4(drawPosition.x, drawPosition.y, drawPosition.z, 1.0f);
                    api::GetSandiumLogger()->Log("Boombox draw: item {} position ({:.2f},{:.2f},{:.2f}) camera ({:.2f},{:.2f},{:.2f}) clip ({:.2f},{:.2f},{:.2f},{:.2f})",
                        i, item.position.x, item.position.y, item.position.z, camera[0], camera[1], camera[2], clip.x, clip.y, clip.z, clip.w);
                    reportedDraw = true;
                }
            }
        }
        glBindVertexArray(oldVao);
        glBindBuffer(GL_ARRAY_BUFFER, oldArrayBuffer);
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActiveTexture);
        glUseProgram(oldProgram);
        glBlendFuncSeparate(oldBlendSrcRgb, oldBlendDstRgb, oldBlendSrcAlpha, oldBlendDstAlpha);
        glDepthMask(oldDepthMask);
        if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (!blend) glDisable(GL_BLEND);
        if (cull) glEnable(GL_CULL_FACE);
    }

    void Shutdown()
    {
        texture.reset(); if (vbo) glDeleteBuffers(1, &vbo); if (vao) glDeleteVertexArrays(1, &vao); if (program) glDeleteProgram(program);
        vbo = vao = program = 0; initialized = false;
    }

    bool IsBoomboxItem(const structs::Item &item)
    {
        if (!addresses::Items.ptr) return false;
        const std::ptrdiff_t index = &item - addresses::Items.ptr;
        return index >= 0 && index < static_cast<std::ptrdiff_t>(boomboxItems.size()) && boomboxItems[static_cast<std::size_t>(index)];
    }
}
