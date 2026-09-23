#include "CustomModels.hpp"

#include "Addresses.hpp"
#include "Flags.hpp"
#include "GeneratedBoomboxModel.hpp"
#include "api/GLUniforms.hpp"
#include "api/Image.hpp"
#include "api/Logging.hpp"
#include "hooks/CreateItem.hpp"
#include "ItemModels.hpp"
#include "TypeManager.hpp"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <subhook.h>

#if _WIN32
#include <Windows.h>
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
        constexpr int M9_BAYONET_TYPE = 47;
        constexpr int MAGNUM_TYPE = 5;
        constexpr int NINE_MM_TYPE = 11;
        bool revolverConfigured = false;
        bool revolverModelLoaded = false;
        bool revolverSpawned = false;
        bool revolverWasInGame = false;
        std::string revolverTypeID = "sandium:revolver";
        bool m9Configured = false;
        bool m9ModelLoaded = false;
        bool m9Spawned = false;
        bool m9WasInGame = false;
        bool m9SpawnRequested = false;
        float m9StabBlend = 0.0f;
        std::string m9TypeID = "sandium:m9_bayonet";
        // Item origin sits at hand + this offset; the flipped mesh's handle
        // straddles z -0.057..+0.037 (center ~-0.01), so z ~+0.01 seats the
        // handle in the fist instead of floating the blade ahead of it.
        constexpr float M9_IDLE_ITEM_POS[3] = {0.02f, -0.03f, 0.01f};
        constexpr float M9_STAB_ITEM_POS[3] = {-0.01f, 0.01f, 0.46f};

        bool ParseM9GripFlag(const std::string &value, structs::CVector3 &grip)
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (std::sscanf(value.c_str(), "%f %f %f", &x, &y, &z) != 3)
                return false;
            grip = structs::CVector3(x, y, z);
            return true;
        }
        constexpr float M9_IDLE_BONE_ROT_DEG[16][3] = {
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {40.0f, -20.0f, 0.0f},
            {0.0f, -10.0f, 0.0f}, {0.0f, -28.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
        };
        constexpr float M9_STAB_BONE_ROT_DEG[16][3] = {
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {40.0f, -40.0f, -6.0f},
            {-8.0f, -50.0f, -5.0f}, {0.0f, -20.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
        };

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

    void LoadSavedGrip();

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
        // hold the revolver like the 9mm: copy the 9mm's equip pose fields
        // (hand offsets, grip drives, gun hold position) over the magnum
        // defaults the clone started from
        for (std::size_t i = 0; i < structs::ItemType::VanillaCount; ++i)
        {
            const std::string name = addresses::ItemTypes[i].GetName();
            if (name.find("9mm") == std::string::npos && name.find("9MM") == std::string::npos)
                continue;
            const auto &src = addresses::ItemTypes[i];
            auto &dst = addresses::ItemTypes[REVOLVER_TYPE];
            dst.handCount = src.handCount;
            dst.rightHandOffset = src.rightHandOffset;
            dst.leftHandOffset = src.leftHandOffset;
            dst.useAlternativeAim = src.useAlternativeAim;
            dst.primaryGripStiffness = src.primaryGripStiffness;
            dst.primaryGripRotation = src.primaryGripRotation;
            dst.secondaryGripStiffness = src.secondaryGripStiffness;
            dst.secondaryGripRotation = src.secondaryGripRotation;
            dst.gunHoldPosition = src.gunHoldPosition;
            // The 9mm grip offset is calibrated to the 9mm mesh; the custom
            // revolver mesh's grip sits 0.075 further back (its centroid is at
            // z -0.109 vs the 9mm's -0.035), so shift the hand back onto it.
            dst.rightHandOffset = structs::CVector3(0.0f, 0.0571f, 0.1057f);
            api::GetSandiumLogger()->Log("Revolver hold pose copied from item type {} ({})",
                                         (int)i, name);
            break;
            LoadSavedGrip();
        }
        GetItemTypeManager()->RegisterID("sandium", "revolver", REVOLVER_TYPE);
        revolverConfigured = true;
        try
        {
            revolverModelLoaded = itemmodels::Set(
                addresses::ItemTypes[REVOLVER_TYPE],
                "sandium/models/revolver/revolver.cmo",
                "sandium/models/revolver/TheGenerals451Tex.png");
        }
        catch (const std::exception &error)
        {
            revolverModelLoaded = false;
            api::GetSandiumLogger()->Log("Revolver model error: {}", error.what());
        }
        api::GetSandiumLogger()->Log("Registered sandium:revolver as native weapon type {}",
                                     REVOLVER_TYPE);
    }

    // live M9 tuning: "m9grip <x> <y> <z>" moves the knife on the hand,
    // "m9rot <deg>" twists it; both persist in sandium_m9grip.txt.
    // (Forward declarations; definitions follow CheckM9Command.)
    void ApplyM9Grip(float x, float y, float z);
    void ApplyM9GripRotation(float r);
    void LoadSavedM9Grip();

    void ConfigureM9BayonetType()
    {
        auto *typeID = addresses::ItemTypes[M9_BAYONET_TYPE].customData.typeIDPtr;
        addresses::ItemTypes[M9_BAYONET_TYPE] = addresses::ItemTypes[NINE_MM_TYPE];
        auto &definition = addresses::ItemTypes[M9_BAYONET_TYPE];
        definition.customData.index = M9_BAYONET_TYPE;
        definition.customData.typeIDPtr = typeID;
        *typeID = m9TypeID;
        definition.SetName("M9 Bayonet");
        definition.isGun = {};
        // Never run the 9mm-derived aim/fire path for the held knife. With
        // useAlternativeAim the renderer locks the mesh to the camera (so it
        // hangs behind the hand instead of riding it) and LMB runs the gun
        // recoil cycle, which tumbles the mesh on a zero-ammo "gun".
        definition.useAlternativeAim = {};
        definition.ammoCount = 0;
        definition.fireRate = 18;
        definition.bulletTypeID = 0;
        definition.bulletVelocity = 0.0f;
        definition.bulletSpread = 0.0f;
        definition.handCount = 1;
        definition.rightHandOffset = structs::CVector3(
            M9_IDLE_ITEM_POS[0], M9_IDLE_ITEM_POS[1], M9_IDLE_ITEM_POS[2]);
        definition.leftHandOffset = structs::CVector3(0.0f, 0.0f, 0.0f);
        definition.gunHoldPosition = structs::CVector3(0.26f, -0.22f, 0.52f);
        definition.primaryGripStiffness = 0.95f;
        // The native grip field is radians.  Feeding it the old value 20.0
        // asked the IK drive to wind the wrist through several complete turns,
        // which is the source of the spinning/disc pose.  Keep tuning values in
        // human-friendly degrees, but only ever give radians to the engine.
        // Keep the model aligned to its original grip; server Lua controls the
        // hand's actual hold/stab orientation through the wrist quaternion.
        definition.primaryGripRotation = glm::radians(-160.0f);
        definition.secondaryGripStiffness = 0.0f;
        definition.secondaryGripRotation = 0.0f;

        GetItemTypeManager()->RegisterID("sandium", "m9_bayonet", M9_BAYONET_TYPE);
        m9Configured = true;
        try
        {
            m9ModelLoaded = itemmodels::Set(
                addresses::ItemTypes[M9_BAYONET_TYPE],
                "sandium/models/m9_bayonet/m9_bayonet.cmo",
                "sandium/models/m9_bayonet/m9_bayonet.png");
        }
        catch (const std::exception &error)
        {
            m9ModelLoaded = false;
            api::GetSandiumLogger()->Log("M9 Bayonet model error: {}", error.what());
        }
        LoadSavedM9Grip();
        api::GetSandiumLogger()->Log("Registered sandium:m9_bayonet as native item type {}",
                                     M9_BAYONET_TYPE);
    }

    // live grip tuning: typing "grip <x> <y> <z>" in chat moves the right
    // hand on the held revolver; the value persists in sandium_grip.txt and is
    // re-applied on every launch until removed.
    void ApplyGrip(float x, float y, float z)
    {
        addresses::ItemTypes[REVOLVER_TYPE].rightHandOffset = structs::CVector3(x, y, z);
        if (std::FILE *f = std::fopen("sandium_grip.txt", "w"))
        {
            std::fprintf(f, "%.4f %.4f %.4f\n", x, y, z);
            std::fclose(f);
        }
    }

    void LoadSavedGrip()
    {
        if (std::FILE *f = std::fopen("sandium_grip.txt", "r"))
        {
            float x, y, z;
            if (std::fscanf(f, "%f %f %f", &x, &y, &z) == 3)
                addresses::ItemTypes[REVOLVER_TYPE].rightHandOffset = structs::CVector3(x, y, z);
            std::fclose(f);
        }
    }

    void CheckGripCommand()
    {
        static char lastCommand[61] = {};
        const char *chat = reinterpret_cast<const char *>(
            reinterpret_cast<const char *>(addresses::Base.ptr) + 0x404A6C + 4ULL * 283785913);
        char lower[61];
        std::size_t i = 0;
        for (; i < 60 && chat[i]; ++i)
            lower[i] = (char)tolower((unsigned char)chat[i]);
        lower[i] = 0;
        if (std::strcmp(lower, lastCommand) == 0)
            return;
        std::strncpy(lastCommand, lower, 60);
        float x, y, z;
        if (std::sscanf(lower, "grip %f %f %f", &x, &y, &z) == 3)
            ApplyGrip(x, y, z);
    }

    void CheckM9Command()
    {
        static char lastCommand[61] = {};
#if _WIN32
        if (GetAsyncKeyState(VK_F8) & 1)
        {
            m9Spawned = false;
            m9SpawnRequested = true;
            api::GetSandiumLogger()->Log("M9 Bayonet spawn requested from F8");
        }
#endif
        if (!addresses::Base.ptr)
            return;
        const char *chat = reinterpret_cast<const char *>(
            reinterpret_cast<const char *>(addresses::Base.ptr) + 0x404A6C + 4ULL * 283785913);
        char lower[61];
        std::size_t i = 0;
        for (; i < 60 && chat[i]; ++i)
            lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(chat[i])));
        lower[i] = 0;
        if (std::strcmp(lower, lastCommand) == 0)
            return;
        std::strncpy(lastCommand, lower, 60);
        if (std::strcmp(lower, "m9") == 0 || std::strcmp(lower, "knife") == 0 ||
            std::strcmp(lower, "/m9") == 0 || std::strcmp(lower, "/knife") == 0)
        {
            m9Spawned = false;
            m9SpawnRequested = true;
            api::GetSandiumLogger()->Log("M9 Bayonet spawn requested from chat command");
        }
        float gx, gy, gz;
        if (std::sscanf(lower, "m9grip %f %f %f", &gx, &gy, &gz) == 3)
            ApplyM9Grip(gx, gy, gz);
        float gr;
        if (std::sscanf(lower, "m9rot %f", &gr) == 1)
            ApplyM9GripRotation(gr);
    }

    // live M9 tuning: "m9grip <x> <y> <z>" moves the knife on the hand,
    // "m9rot <deg>" twists it; both persist in sandium_m9grip.txt.
    void ApplyM9Grip(float x, float y, float z)
    {
        addresses::ItemTypes[M9_BAYONET_TYPE].rightHandOffset = structs::CVector3(x, y, z);
        const float degrees = glm::degrees(
            addresses::ItemTypes[M9_BAYONET_TYPE].primaryGripRotation);
        if (std::FILE *f = std::fopen("sandium_m9grip.txt", "w"))
        {
            std::fprintf(f, "%.4f %.4f %.4f %.4f\n", x, y, z, degrees);
            std::fclose(f);
        }
        api::GetSandiumLogger()->Log("M9 grip set to {:.4f} {:.4f} {:.4f}", x, y, z);
    }

    void ApplyM9GripRotation(float degrees)
    {
        addresses::ItemTypes[M9_BAYONET_TYPE].primaryGripRotation = glm::radians(degrees);
        const auto off = addresses::ItemTypes[M9_BAYONET_TYPE].rightHandOffset;
        if (std::FILE *f = std::fopen("sandium_m9grip.txt", "w"))
        {
            // Persist degrees so existing files and the m9rot command remain
            // understandable.  Conversion happens only at the engine boundary.
            std::fprintf(f, "%.4f %.4f %.4f %.4f\n", off.x, off.y, off.z, degrees);
            std::fclose(f);
        }
        api::GetSandiumLogger()->Log("M9 grip rotation set to {:.2f} degrees ({:.4f} radians)",
                                     degrees, glm::radians(degrees));
    }

    void LoadSavedM9Grip()
    {
        if (std::FILE *f = std::fopen("sandium_m9grip.txt", "r"))
        {
            float x, y, z, r = 0.0f;
            const int got = std::fscanf(f, "%f %f %f %f", &x, &y, &z, &r);
            if (got >= 3)
                addresses::ItemTypes[M9_BAYONET_TYPE].rightHandOffset = structs::CVector3(x, y, z);
            if (got == 4)
                addresses::ItemTypes[M9_BAYONET_TYPE].primaryGripRotation = glm::radians(r);
            std::fclose(f);
        }
    }

    void UpdateRevolver()
    {
        CheckGripCommand();
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

    void UpdateM9Bayonet()
    {
        CheckM9Command();
        if (!m9Configured || !addresses::IsInGame.ptr)
            return;

        const bool inGame = *addresses::IsInGame;
        if (!inGame)
        {
            m9Spawned = false;
            m9WasInGame = false;
            m9SpawnRequested = false;
            m9StabBlend = 0.0f;
            return;
        }
        if (!m9WasInGame)
        {
            m9Spawned = false;
            m9WasInGame = true;
        }

        structs::Human *localHuman = nullptr;
        int localHumanID = -1;
        for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
        {
            auto &human = addresses::Humans[h];
            if (human.isActive.b1)
            {
                localHuman = &human;
                localHumanID = static_cast<int>(h);
                break;
            }
        }
        if (!localHuman)
            return;

        // The server publishes both offsets. Select locally from LMB so the
        // mesh moves with the stab on the same frame without touching any
        // rigid-body rotation, axis, velocity, or torque.
        bool holdingM9 = false;
        for (std::size_t i = 0; i < structs::Item::VanillaCount; ++i)
        {
            const auto &item = addresses::Items[i];
            if (item.isActive.b1 && item.typeID == M9_BAYONET_TYPE &&
                item.parentHumanID == localHumanID)
            {
                holdingM9 = true;
                break;
            }
        }
        const bool stabbing = holdingM9 && (localHuman->inputFlags & 1u) != 0;
        structs::CVector3 streamedGrip;
        const std::string gripFlag = flags::Get(stabbing ? "bayonet_gripstab" : "bayonet_grip");
        if (ParseM9GripFlag(gripFlag, streamedGrip))
            addresses::ItemTypes[M9_BAYONET_TYPE].rightHandOffset = streamedGrip;
        const std::string rotationFlag = flags::Get(
            stabbing ? "bayonet_gripstabrot" : "bayonet_griprot");
        float streamedRotation = 0.0f;
        if (std::sscanf(rotationFlag.c_str(), "%f", &streamedRotation) == 1)
            addresses::ItemTypes[M9_BAYONET_TYPE].primaryGripRotation =
                glm::radians(streamedRotation);

        // Hand placement belongs to the server (arm IK). The client keeps a
        // single static item offset and never animates it: no per-frame
        // sliding, no aim/fire path (see ConfigureM9BayonetType).
        // Spawn only on explicit request (m9/knife chat, F8). Auto-spawning
        // on join duplicates the server's knife with a loose second blade.
        if (m9Spawned || !m9ModelLoaded || !m9SpawnRequested)
            return;

        int slot = -1;
        for (int s = 0; s < 6; ++s)
        {
            if (s != 2 && localHuman->inventorySlots[s].numberOfPlaces == 0)
            {
                slot = s;
                break;
            }
        }
        if (slot < 0)
        {
            if (!m9SpawnRequested)
                return;
            api::GetSandiumLogger()->Log("M9 Bayonet inventory full; spawning on ground instead");
        }

        structs::CVector3 position = localHuman->position;
        if (slot < 0)
        {
            const float yaw = localHuman->viewYaw;
            position.x += std::sin(yaw) * 1.2f;
            position.y += 0.25f;
            position.z += std::cos(yaw) * 1.2f;
        }
        structs::CVector3 velocity{};
        structs::COrientation orientation{};
        int itemID = -1;
        {
            subhook::ScopedHookRemove remove(createItemHook);
            itemID = addresses::CreateItemFunc(M9_BAYONET_TYPE, &position, &velocity, &orientation);
        }
        if (itemID < 0 || itemID >= static_cast<int>(structs::Item::VanillaCount))
            return;
        addresses::Items[itemID].ammoCount = 0;
        if (slot >= 0 && (!addresses::AttachItemFunc.ptr ||
            addresses::AttachItemFunc(itemID, -1, localHumanID, slot) == 0)
        )
        {
            addresses::Items[itemID].isActive = {};
            api::GetSandiumLogger()->Log("M9 Bayonet error: native inventory attachment failed for item {}", itemID);
            return;
        }
        m9Spawned = true;
        m9SpawnRequested = false;
        api::GetSandiumLogger()->Log("Spawned M9 Bayonet item {} for human {} in slot {}",
                                     itemID, localHumanID, slot);
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
