#include "Lighting.hpp"

#include "GLUniforms.hpp"
#include "BloodMarks.hpp"
#include "../Diagnostics.hpp"
#include "../CustomModels.hpp"
#include "../LocalHuman.hpp"
#include "Sound.hpp"

#include "../Addresses.hpp"
#include "../Flags.hpp"
#include "../structs/Human.hpp"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <mutex>
#include <vector>

#if _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace api
{
    namespace
    {
        constexpr std::size_t MAX_LIGHTS = 512;

        struct FlashlightPowerState
        {
            std::array<bool, structs::Item::VanillaCount> known{};
            std::array<bool, structs::Item::VanillaCount> on{};
        };

        std::mutex &RegistryMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::vector<std::shared_ptr<Light>> &Registry()
        {
            static std::vector<std::shared_ptr<Light>> registry;
            return registry;
        }

        // Own GL objects (core profile has no default VAO — GrainShader lesson).
        struct Renderer
        {
            bool ready = false;
            GLuint program = 0;
            GLuint vao = 0;
            GLuint vbo = 0;
            GLint colorLocation = -1;
            GLint intensityLocation = -1;
        };

        Renderer &GetRenderer()
        {
            static Renderer renderer;
            return renderer;
        }

        const char *vertexSource =
            "#version 330 core\n"
            "layout(location=0) in vec3 inposition;\n"
            "layout(location=1) in vec2 inuv;\n"
            "out vec2 uv;\n"
            "void main()\n"
            "{\n"
            "    uv=inuv;\n"
            "    gl_Position=vec4(inposition,1.0);\n"
            "}\n";

        const char *fragmentSource =
            "#version 330 core\n"
            "uniform vec3 color;\n"
            "uniform float intensity;\n"
            "in vec2 uv;\n"
            "out vec4 outcolor;\n"
            "void main()\n"
            "{\n"
            "    float d=length(uv*2.0-1.0);\n"
            "    float falloff=pow(clamp(1.0-d,0.0,1.0),1.6);\n"
            "    outcolor=vec4(color*intensity*falloff,1.0);\n"
            "}\n";

        void Initialize()
        {
            Renderer &renderer = GetRenderer();
            if (renderer.ready)
                return;

            auto compile = [](GLenum type, const char *source) {
                GLuint shader = glCreateShader(type);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint status = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
                if (!status)
                {
                    glDeleteShader(shader);
                    return 0u;
                }
                return shader;
            };

            const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
            const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
            if (!vertex || !fragment)
            {
                diag::Log("lighting", "shader compile failed");
                return;
            }
            renderer.program = glCreateProgram();
            glAttachShader(renderer.program, vertex);
            glAttachShader(renderer.program, fragment);
            glLinkProgram(renderer.program);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            GLint status = 0;
            glGetProgramiv(renderer.program, GL_LINK_STATUS, &status);
            if (!status)
                return;

            glGenVertexArrays(1, &renderer.vao);
            glGenBuffers(1, &renderer.vbo);
            glBindVertexArray(renderer.vao);
            glBindBuffer(GL_ARRAY_BUFFER, renderer.vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            renderer.colorLocation = glGetUniformLocation(renderer.program, "color");
            renderer.intensityLocation = glGetUniformLocation(renderer.program, "intensity");
            renderer.ready = true;
        }

        const structs::Human *FirstActiveHuman(int &humanID)
        {
            for (std::size_t h = 0; h < structs::Human::VanillaCount; ++h)
            {
                if (addresses::Humans[h].isActive.b1)
                {
                    humanID = static_cast<int>(h);
                    return &addresses::Humans[h];
                }
            }
            return nullptr;
        }

        const structs::Item *HeldFlashlight(const structs::Human &human)
        {
            if (!addresses::Items.ptr)
                return nullptr;
            for (int hand = 0; hand < 2; ++hand)
            {
                const auto &slot = human.inventorySlots[hand];
                if (slot.numberOfPlaces <= 0)
                    continue;
                const int itemIDs[2] = {slot.firstPlaceItemID, slot.secondPlaceItemID};
                for (const int itemID : itemIDs)
                    if (itemID >= 0 && itemID < static_cast<int>(structs::Item::VanillaCount) &&
                        addresses::Items[itemID].isActive.b1 &&
                        addresses::Items[itemID].typeID == 48)
                        return &addresses::Items[itemID];
            }
            return nullptr;
        }

        void PlayFlashlightClick(bool turnedOn)
        {
            static bool attempted = false;
            static std::shared_ptr<Sound> onSound, offSound;
            if (!attempted)
            {
                attempted = true;
                try
                {
                    onSound = Sound::Load("sandium/Assets/flashlight/on.wav");
                    offSound = Sound::Load("sandium/Assets/flashlight/off.wav");
                }
                catch (const std::exception &error)
                {
                    diag::Log("lighting", "flashlight click unavailable: %s", error.what());
                }
            }
            const auto &sound = turnedOn ? onSound : offSound;
            if (sound)
                sound->Play(0.65f);
        }

        // 6 vertices (two triangles), position = clip space xyz, uv per corner.
        void PushQuad(std::vector<float> &buffer,
                      const float corners[4][3], float zDiv[4])
        {
            const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            const int indices[6] = {0, 1, 2, 0, 2, 3};
            for (int index : indices)
            {
                const float w = zDiv[index];
                buffer.push_back(corners[index][0] / w);
                buffer.push_back(corners[index][1] / w);
                buffer.push_back(corners[index][2] / w);
                buffer.push_back(uvs[index][0]);
                buffer.push_back(uvs[index][1]);
            }
        }

        // Transform a world corner by the view-projection matrix.
        void ProjectCorner(const glm::mat4 &viewProjection, const glm::vec3 &corner,
                           float out[3], float &w)
        {
            const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
            out[0] = clip.x;
            out[1] = clip.y;
            out[2] = clip.z;
            w = clip.w;
        }
    }

    Light::~Light() = default;

    void Light::Destroy() { lighting::Destroy(this); }
    void Light::SetPosition(float px, float py, float pz) { x = px; y = py; z = pz; follow = false; }
    void Light::SetColor(float r, float g, float b) { red = r; green = g; blue = b; }
    void Light::SetRadius(float radiusValue) { radius = radiusValue; }
    void Light::SetIntensity(float intensityValue) { intensity = intensityValue; }
    void Light::SetFollow(int human, float ox, float oy, float oz)
    {
        humanIndex = human;
        offsetX = ox;
        offsetY = oy;
        offsetZ = oz;
        follow = human >= 0;
    }
    void Light::SetGroundPool(bool enable) { groundPool = enable; }
    void Light::SetSourceVisible(bool enable) { source = enable; }
    void Light::SetDirection(float dx, float dy, float dz)
    {
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 0.0001f)
            return;
        dirX = dx / length;
        dirY = dy / length;
        dirZ = dz / length;
    }
    void Light::SetCone(float angleDegrees)
    {
        coneDegrees = std::clamp(angleDegrees, 1.0f, 180.0f);
    }

    namespace lighting
    {
        float masterBrightness = 1.0f;

        void SetBrightness(float brightness)
        {
            masterBrightness = std::clamp(brightness, 0.1f, 10.0f);
        }

        float GetBrightness() { return masterBrightness; }

        void Register(const std::shared_ptr<Light> &light)
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            auto &registry = Registry();
            if (registry.size() >= MAX_LIGHTS)
                registry.erase(registry.begin());
            registry.push_back(light);
        }

        void Destroy(Light *light)
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            auto &registry = Registry();
            for (auto it = registry.begin(); it != registry.end(); ++it)
            {
                if (it->get() == light)
                {
                    registry.erase(it);
                    return;
                }
            }
        }

        void Clear()
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            Registry().clear();
        }

        void DrawFrame()
        {
#if _WIN32
            static FlashlightPowerState flashlightPower;
            if (!addresses::IsInGame.ptr || !addresses::IsInGame.ptr->b1)
            {
                flashlightPower = FlashlightPowerState{};
                bloodmarks::Clear();
                return;
            }

        const int localHumanID = localhuman::Find().second;
        std::array<int, structs::Item::VanillaCount> heldByHuman{};
        std::array<bool, structs::Item::VanillaCount> powerChanged{};
        heldByHuman.fill(-1);
        if (addresses::Items.ptr)
        {
            for (std::size_t itemID = 0; itemID < structs::Item::VanillaCount; ++itemID)
            {
                const auto &item = addresses::Items[itemID];
                const bool isFlashlight = item.isActive.b1 && item.typeID == 48;
                if (!isFlashlight)
                {
                    flashlightPower.known[itemID] = false;
                    continue;
                }
                const std::string powerFlag = flags::Get("flashlight_" + std::to_string(itemID));
                const bool powered = powerFlag == "1" ||
                    (powerFlag != "0" && item.ammoCount > 0);
                if (!flashlightPower.known[itemID])
                {
                    flashlightPower.known[itemID] = true;
                    flashlightPower.on[itemID] = powered;
                }
                else if (flashlightPower.on[itemID] != powered)
                {
                    flashlightPower.on[itemID] = powered;
                    powerChanged[itemID] = true;
                }
            }
        }
        if (addresses::Humans.ptr && addresses::Items.ptr)
        {
            for (std::size_t humanID = 0; humanID < structs::Human::VanillaCount; ++humanID)
            {
                const auto &human = addresses::Humans[humanID];
                if (!human.isActive.b1)
                    continue;
                const auto *heldFlashlight = HeldFlashlight(human);
                const int itemID = heldFlashlight
                    ? static_cast<int>(heldFlashlight - addresses::Items.ptr) : -1;
                if (itemID >= 0)
                    heldByHuman[itemID] = static_cast<int>(humanID);
                if (itemID >= 0 && powerChanged[itemID] &&
                    static_cast<int>(humanID) == localHumanID)
                    PlayFlashlightClick(flashlightPower.on[itemID]);
            }
        }

        std::vector<std::shared_ptr<Light>> lights;
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            lights = Registry();
        }

        // Feed the ShaderLights-injected uniforms for the next world pass —
        // always, even with zero lights, so stale values get cleared. The
        // shader has 32 slots; when more lights exist, the camera-nearest
        // win (a far light contributes almost nothing).
        constexpr int kShaderSlots = 32;
        if (!glcap::HasViewPosition())
            return;
        const float *cameraForUniforms = glcap::ViewPosition();

        struct ResolvedLight
        {
            float x, y, z;
            float radius;
            float r, g, b;
            float intensity;
            float distanceSquared;
            bool source;
            bool glare;
            float dirX, dirY, dirZ;
            float coneDegrees;
        };
        std::vector<ResolvedLight> resolved;
        resolved.reserve(lights.size() + structs::Human::VanillaCount);
        std::array<bool, structs::Human::VanillaCount> scriptedSpot{};
        for (const auto &light : lights)
        {
            if (!light)
                continue;
            glm::vec3 position(light->x, light->y, light->z);
            glm::vec3 direction(light->dirX, light->dirY, light->dirZ);
            if (light->follow)
            {
                if (light->humanIndex < 0 ||
                    light->humanIndex >= static_cast<int>(structs::Human::VanillaCount) ||
                    !addresses::Humans[light->humanIndex].isActive.b1)
                    continue;
                const auto &human = addresses::Humans[light->humanIndex];
                position = glm::vec3(
                    human.position.x + light->offsetX,
                    human.position.y + light->offsetY,
                    human.position.z + light->offsetZ);
                // A following SPOT aims along the followed human's view, so
                // one server-published "follow + cone" light renders every
                // player's flashlight beam from networked view angles.
                if (light->coneDegrees < 180.0f)
                {
                    scriptedSpot[light->humanIndex] = true;
                    const float yaw = human.viewYaw;
                    const float pitch = human.viewPitch;
                    direction = glm::normalize(glm::vec3(
                        std::sin(yaw) * std::cos(pitch),
                        -std::sin(pitch),
                        std::cos(yaw) * std::cos(pitch)));
                    // Start the beam just ahead of the face so the cone does
                    // not spend its brightest part inside the player's head.
                    position += direction * 0.20f;
                }
            }
            ResolvedLight entry{};
            entry.x = position.x;
            entry.y = position.y;
            entry.z = position.z;
            entry.radius = light->radius > 0.05f ? light->radius : 0.05f;
            entry.r = light->red;
            entry.g = light->green;
            entry.b = light->blue;
            entry.intensity = light->intensity * masterBrightness;
            entry.source = light->source;
            entry.glare = false;
            entry.dirX = direction.x;
            entry.dirY = direction.y;
            entry.dirZ = direction.z;
            entry.coneDegrees = light->coneDegrees;
            const float dx = position.x - cameraForUniforms[0];
            const float dy = position.y - cameraForUniforms[1];
            const float dz = position.z - cameraForUniforms[2];
            entry.distanceSquared = dx * dx + dy * dy + dz * dz;
            resolved.push_back(entry);
        }

        // The beam belongs to the item, not its current holder. A dropped
        // flashlight stays on until its next holder switches it off.
        if (addresses::Items.ptr)
        {
            for (std::size_t itemID = 0; itemID < structs::Item::VanillaCount; ++itemID)
            {
                const auto &flashlight = addresses::Items[itemID];
                if (!flashlight.isActive.b1 || flashlight.typeID != 48 ||
                    !flashlightPower.on[itemID])
                    continue;
                const int holderID = heldByHuman[itemID];
                const bool dropped = flashlight.parentHumanID < 0 && flashlight.parentItemID < 0;
                if (holderID < 0 && !dropped)
                    continue;
                if (holderID >= 0 && scriptedSpot[holderID])
                    continue;
                // The visible world model uses the rigid body's render
                // transform. Item::position/orientation can lag or differ,
                // leaving a dropped flashlight's beam beside its lens.
                structs::CVector3 lensPosition = flashlight.position;
                structs::COrientation lensOrientation = flashlight.orientation;
                if (dropped && addresses::Base.ptr && flashlight.rigidBodyID >= 0 &&
                    flashlight.rigidBodyID < 0x2000)
                {
                    const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
                    const auto renderObject = base + 0x449816e0ull +
                        static_cast<std::uintptr_t>(flashlight.rigidBodyID) * 0xbcull;
                    const float *position = reinterpret_cast<const float *>(renderObject + 0x18);
                    const float *right = reinterpret_cast<const float *>(renderObject + 0x3c);
                    const float *up = reinterpret_cast<const float *>(renderObject + 0x48);
                    const float *back = reinterpret_cast<const float *>(renderObject + 0x54);
                    const float deltaX = position[0] - flashlight.position.x;
                    const float deltaY = position[1] - flashlight.position.y;
                    const float deltaZ = position[2] - flashlight.position.z;
                    const float backLengthSquared = back[0] * back[0] +
                        back[1] * back[1] + back[2] * back[2];
                    if (std::isfinite(position[0]) && std::isfinite(position[1]) &&
                        std::isfinite(position[2]) &&
                        std::isfinite(right[0]) && std::isfinite(right[1]) &&
                        std::isfinite(right[2]) &&
                        std::isfinite(up[0]) && std::isfinite(up[1]) &&
                        std::isfinite(up[2]) &&
                        std::isfinite(back[0]) && std::isfinite(back[1]) &&
                        std::isfinite(back[2]) &&
                        backLengthSquared > 0.5f && backLengthSquared < 1.5f &&
                        deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ < 4.0f)
                    {
                        lensPosition = structs::CVector3(position[0], position[1], position[2]);
                        lensOrientation.right = structs::CVector3(right[0], right[1], right[2]);
                        lensOrientation.up = structs::CVector3(up[0], up[1], up[2]);
                        lensOrientation.back = structs::CVector3(back[0], back[1], back[2]);
                    }
                }
                const auto &backAxis = lensOrientation.back;
                const auto &rightAxis = lensOrientation.right;
                const auto &upAxis = lensOrientation.up;
                // The flashlight CMO's reflective lens and front rim are at
                // local -Z; +Z is the tail. Follow the item's orientation so
                // wrist pose changes carry the beam with the visible lens.
                const glm::vec3 worldDirection(-backAxis.x, -backAxis.y, -backAxis.z);
                const float directionLength = glm::length(worldDirection);
                if (!std::isfinite(directionLength) || directionLength < 0.5f)
                    continue;
                const glm::vec3 direction = worldDirection / directionLength;
                // Recess the light slightly into the front of the flashlight
                // so its circular pool starts behind the visible lens edge.
                const float lightOffset = 0.1723f * custommodels::FlashlightMeshScale - 0.04f;
                const auto grip = custommodels::GetFlashlightGripOffset();
                const glm::vec3 meshOffset =
                    glm::vec3(rightAxis.x, rightAxis.y, rightAxis.z) * grip.x +
                    glm::vec3(upAxis.x, upAxis.y, upAxis.z) * grip.y +
                    glm::vec3(backAxis.x, backAxis.y, backAxis.z) * grip.z;
                const glm::vec3 position(
                    lensPosition.x + meshOffset.x + direction.x * lightOffset,
                    lensPosition.y + meshOffset.y + direction.y * lightOffset,
                    lensPosition.z + meshOffset.z + direction.z * lightOffset);
                if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                    !std::isfinite(position.z))
                    continue;
                ResolvedLight entry{};
                entry.x = position.x; entry.y = position.y; entry.z = position.z;
                entry.radius = 30.0f;
                entry.r = 0.88f; entry.g = 0.94f; entry.b = 1.0f;
                const float modeBrightness = flags::Get("active_mode") == "hideAndSeek"
                    ? 1.6f : 1.0f;
                entry.intensity = 1.25f * modeBrightness * masterBrightness;
                entry.source = false;
                entry.glare = true;
                entry.dirX = direction.x; entry.dirY = direction.y; entry.dirZ = direction.z;
                entry.coneDegrees = 30.0f;
                const float dx = position.x - cameraForUniforms[0];
                const float dy = position.y - cameraForUniforms[1];
                const float dz = position.z - cameraForUniforms[2];
                entry.distanceSquared = dx * dx + dy * dy + dz * dz;
                resolved.push_back(entry);
            }
        }
        std::sort(resolved.begin(), resolved.end(),
                  [](const ResolvedLight &a, const ResolvedLight &b) {
                      return a.distanceSquared < b.distanceSquared;
                  });

        float points[kShaderSlots * 4] = {};
        float colors[kShaderSlots * 4] = {};
        float spots[kShaderSlots * 4] = {};
        bool slotSources[kShaderSlots] = {};
        bool slotGlares[kShaderSlots] = {};
        int slot = 0;
        for (const auto &entry : resolved)
        {
            if (slot >= kShaderSlots)
                break;
            points[slot * 4 + 0] = entry.x;
            points[slot * 4 + 1] = entry.y;
            points[slot * 4 + 2] = entry.z;
            points[slot * 4 + 3] = entry.radius;
            colors[slot * 4 + 0] = entry.r;
            colors[slot * 4 + 1] = entry.g;
            colors[slot * 4 + 2] = entry.b;
            colors[slot * 4 + 3] = entry.intensity;
            spots[slot * 4 + 0] = entry.dirX;
            spots[slot * 4 + 1] = entry.dirY;
            spots[slot * 4 + 2] = entry.dirZ;
            // cos(halfAngle); -2 marks an omnidirectional light, keeping
            // even very narrow cones distinct from the sentinel.
            spots[slot * 4 + 3] = entry.coneDegrees >= 180.0f
                ? -2.0f
                : std::cos(entry.coneDegrees * 3.14159265f / 360.0f);
            slotSources[slot] = entry.source;
            slotGlares[slot] = entry.glare;
            ++slot;
        }

        int fedPrograms = 0;
        float bloodValues[24 * 4] = {};
        bloodmarks::FillUniforms(bloodValues, 24);
        for (const unsigned int program : glcap::CapturedPrograms())
        {
            // Array uniforms are addressed as "name[0]" per the GLSL spec.
            const GLint lightLocation = glGetUniformLocation(program, "lightpoints[0]");
            if (lightLocation < 0)
                continue;
            GLint oldProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
            glUseProgram(program);
            glUniform4fv(lightLocation, kShaderSlots, points);
            const GLint colorLocation = glGetUniformLocation(program, "lightcolors[0]");
            if (colorLocation >= 0)
                glUniform4fv(colorLocation, kShaderSlots, colors);
            const GLint spotLocation = glGetUniformLocation(program, "lightspots[0]");
            if (spotLocation >= 0)
                glUniform4fv(spotLocation, kShaderSlots, spots);
            const GLint bloodLocation = glGetUniformLocation(program, "bloodmarks[0]");
            if (bloodLocation >= 0)
                glUniform4fv(bloodLocation, 24, bloodValues);
            const GLint viewLocation = glGetUniformLocation(program, "viewposition");
            if (viewLocation >= 0)
                glUniform3f(viewLocation, cameraForUniforms[0], cameraForUniforms[1], cameraForUniforms[2]);
            glUseProgram(oldProgram);
            ++fedPrograms;
        }

        diag::Log("lighting", "lights=%zu slots=%d fedPrograms=%d",
                  lights.size(), slot, fedPrograms);

        // Draw visible light sources against the world's depth buffer so
        // walls can hide the lens and its glare.
        int bulbs = 0;
        if (glcap::HasViewProjection())
        {
            const float *matrix = glcap::ViewProjectionMatrix();
            const bool transposed = glcap::MatrixTransposed();
            const auto project = [&](const float point[3], float ndc[3]) {
                float clip[4];
                for (int r = 0; r < 4; ++r)
                    clip[r] = transposed
                        ? matrix[r * 4 + 0] * point[0] + matrix[r * 4 + 1] * point[1] + matrix[r * 4 + 2] * point[2] + matrix[r * 4 + 3]
                        : matrix[0 * 4 + r] * point[0] + matrix[1 * 4 + r] * point[1] + matrix[2 * 4 + r] * point[2] + matrix[3 * 4 + r];
                if (clip[3] < 0.05f)
                    return false;
                ndc[0] = clip[0] / clip[3];
                ndc[1] = clip[1] / clip[3];
                ndc[2] = clip[2] / clip[3];
                return true;
            };

            // Save state BEFORE Initialize(): it binds (and finally unbinds)
            // its own objects, and restoring a post-Initialize snapshot would
            // clobber the game's VAO with 0 — core profile then draws nothing
            // and the screen goes black.
            GLint oldProgram = 0, oldVao = 0, oldArrayBuffer = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
            GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE);
            GLint oldBlendSrcRgb = 0, oldBlendDstRgb = 0, oldBlendSrcAlpha = 0, oldBlendDstAlpha = 0;
            GLint oldDepthFunc = 0;
            GLint viewport[4] = {};
            GLint depthBits = 0;
            GLboolean oldDepthMask = GL_TRUE;
            glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);
            glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(0x0D56 /* GL_DEPTH_BITS */, &depthBits);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);

            Initialize();
            Renderer &renderer = GetRenderer();
            if (renderer.ready)
            {
                glUseProgram(renderer.program);
                glBindVertexArray(renderer.vao);
                glBindBuffer(GL_ARRAY_BUFFER, renderer.vbo);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(oldDepthFunc == GL_GREATER || oldDepthFunc == GL_GEQUAL
                    ? GL_GEQUAL : GL_LEQUAL);
                glEnable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glBlendFunc(GL_ONE, GL_ONE);
                glDepthMask(GL_FALSE);

                const auto drawGlow = [&](float x, float y, float z,
                                          float halfWidth, float halfHeight, float strength) {
                    glUniform1f(renderer.intensityLocation, strength);
                    const float quad[6 * 5] = {
                        x - halfWidth, y - halfHeight, z, 0.0f, 0.0f,
                        x + halfWidth, y - halfHeight, z, 1.0f, 0.0f,
                        x + halfWidth, y + halfHeight, z, 1.0f, 1.0f,
                        x - halfWidth, y - halfHeight, z, 0.0f, 0.0f,
                        x + halfWidth, y + halfHeight, z, 1.0f, 1.0f,
                        x - halfWidth, y + halfHeight, z, 0.0f, 1.0f,
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                };

                for (int i = 0; i < slot; ++i)
                {
                    if (!slotSources[i] && !slotGlares[i])
                        continue;
                    if (slotSources[i] && depthBits <= 0)
                        continue;
                    // The HUD pass can bind a different depth buffer from
                    // the world pass. Flashlight glare uses the level ray
                    // below for wall occlusion instead of that buffer.
                    if (slotGlares[i] && (!addresses::LineIntersectLevelFunc.ptr ||
                                          !addresses::LineIntersectResult.ptr))
                        continue;
                    const float position[3] = {points[i * 4 + 0], points[i * 4 + 1], points[i * 4 + 2]};
                    const float markerPosition[3] = {
                        // The beam origin is recessed into the barrel. Put
                        // the visible glint past the lens so the mesh does
                        // not occlude its own source in the depth buffer.
                        position[0] + (slotGlares[i] ? spots[i * 4 + 0] * 0.065f : 0.0f),
                        position[1] + (slotGlares[i] ? spots[i * 4 + 1] * 0.065f : 0.0f),
                        position[2] + (slotGlares[i] ? spots[i * 4 + 2] * 0.065f : 0.0f)
                    };
                    float center[3];
                    if (!project(markerPosition, center))
                        continue;
                    if (addresses::LineIntersectLevelFunc.ptr && addresses::LineIntersectResult.ptr)
                    {
                        const structs::LineIntersectResult saved = *addresses::LineIntersectResult.ptr;
                        structs::CVector3 rayStart(cameraForUniforms[0], cameraForUniforms[1], cameraForUniforms[2]);
                        structs::CVector3 rayEnd(markerPosition[0], markerPosition[1], markerPosition[2]);
                        const bool hit = addresses::LineIntersectLevelFunc(&rayStart, &rayEnd, 1) != 0;
                        const structs::LineIntersectResult result = *addresses::LineIntersectResult.ptr;
                        *addresses::LineIntersectResult.ptr = saved;
                        const float rayLength = glm::length(glm::vec3(
                            rayEnd.x - rayStart.x, rayEnd.y - rayStart.y, rayEnd.z - rayStart.z));
                        const float hitLength = glm::length(glm::vec3(
                            result.position.x - rayStart.x, result.position.y - rayStart.y,
                            result.position.z - rayStart.z));
                        if (hit && hitLength + 0.08f < rayLength)
                            continue;
                    }
                    float intensity = 0.9f;
                    float hx = 0.0f, hy = 0.0f;
                    if (slotGlares[i])
                    {
                        // A visible lens near the crosshair dazzles the viewer.
                        // The angle test is broad enough for a flashlight lying
                        // on the ground, whose cone can skim past the camera.
                        const glm::vec3 source(position[0], position[1], position[2]);
                        const glm::vec3 camera(cameraForUniforms[0], cameraForUniforms[1], cameraForUniforms[2]);
                        const glm::vec3 toCamera = camera - source;
                        const float distance = glm::length(toCamera);
                        if (distance < 0.06f || distance > 30.0f)
                            continue;
                        const glm::vec3 beamDirection(spots[i * 4 + 0], spots[i * 4 + 1], spots[i * 4 + 2]);
                        const float inBeam = glm::dot(toCamera / distance, beamDirection);
                        const float centerDistance = std::sqrt(center[0] * center[0] + center[1] * center[1]);
                        if (center[2] < -1.0f || center[2] > 1.0f)
                            continue;
                        const float facing = std::clamp((inBeam - 0.35f) / 0.65f, 0.0f, 1.0f);
                        const float centered = std::clamp((0.75f - centerDistance) / 0.75f, 0.0f, 1.0f);
                        const float distanceFade = std::clamp(1.0f - distance / 30.0f, 0.0f, 1.0f);
                        float closeFactor = std::clamp((2.5f - distance) / 2.5f, 0.0f, 1.0f);
                        closeFactor = closeFactor * closeFactor * (3.0f - 2.0f * closeFactor);
                        // A small powered lens remains visible off-axis;
                        // the large glare appears when its beam faces the
                        // viewer and grows as the lens approaches.
                        const float direct = facing * centered;
                        intensity = (0.14f + (0.95f + 0.35f * closeFactor) * direct)
                            * distanceFade;
                        hx = hy = std::clamp(
                            0.045f + direct * (0.275f / (1.0f + distance * 0.12f)
                                + 0.55f * closeFactor), 0.035f, 0.85f);
                    }
                    else
                    {
                        // Screen half-extents from a world-sized offset so
                        // the source bulb scales with distance.
                        const float size = 0.25f + points[i * 4 + 3] * 0.04f;
                        float ex[3], ey[3];
                        const float pointX[3] = {position[0] + size, position[1], position[2]};
                        const float pointY[3] = {position[0], position[1] + size, position[2]};
                        if (!project(pointX, ex) || !project(pointY, ey))
                            continue;
                        hx = std::min(0.15f, std::max(0.012f, std::abs(ex[0] - center[0])));
                        hy = std::min(0.15f, std::max(0.012f, std::abs(ey[1] - center[1])));
                    }

                    // Bright core of the light's own color.
                    const float peak = std::max(colors[i * 4 + 0], std::max(colors[i * 4 + 1], colors[i * 4 + 2]));
                    const float scale = peak > 0.001f ? 1.0f / peak : 1.0f;
                    glUniform3f(renderer.colorLocation,
                                colors[i * 4 + 0] * scale, colors[i * 4 + 1] * scale, colors[i * 4 + 2] * scale);
                    if (slotGlares[i]) glDisable(GL_DEPTH_TEST);
                    else glEnable(GL_DEPTH_TEST);
                    drawGlow(center[0], center[1], center[2], hx, hy, intensity);
                    if (slotSources[i]) ++bulbs;
                }
            }

            // Always restore — Initialize() itself may have changed bindings
            // even when the renderer ended up not ready.
            glBindBuffer(GL_ARRAY_BUFFER, oldArrayBuffer);
            glBindVertexArray(oldVao);
            glUseProgram(oldProgram);
            glBlendFuncSeparate(oldBlendSrcRgb, oldBlendDstRgb, oldBlendSrcAlpha, oldBlendDstAlpha);
            glDepthFunc(oldDepthFunc);
            glDepthMask(oldDepthMask);
            if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        }
        if (bulbs > 0)
            diag::Log("lighting", "bulbs=%d", bulbs);
#endif
    }

    void ProbeFrame()
    {
#if _WIN32
        static long long frame = 0;
        ++frame;
        if (frame % 90 != 0)
            return;
        if (std::FILE *flag = std::fopen("sandium_lightprobe.txt", "r"))
            std::fclose(flag);
        else
            return;

        // Read a coarse brightness grid off the frame about to be presented.
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        if (viewport[2] < 64 || viewport[3] < 64)
            return;
        GLint readFb = 0, readBuf = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
        glGetIntegerv(GL_READ_BUFFER, &readBuf);
        // At swap time the composited frame is in the default back buffer.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);

        constexpr int kCols = 16, kRows = 9;
        struct Cell { float green; float red; float ndcX, ndcY; };
        Cell cells[kCols * kRows] = {};
        unsigned char pixel[4] = {};
        for (int row = 0; row < kRows; ++row)
        {
            for (int col = 0; col < kCols; ++col)
            {
                const int x = viewport[0] + (col * 2 + 1) * viewport[2] / (kCols * 2);
                const int y = viewport[1] + (row * 2 + 1) * viewport[3] / (kRows * 2);
                glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                Cell &cell = cells[row * kCols + col];
                // Channel dominance isolates lit pixels from white sunlight.
                const float r = pixel[0] / 255.0f, g = pixel[1] / 255.0f, b = pixel[2] / 255.0f;
                cell.green = g - std::max(r, b);
                cell.red = r - std::max(g, b);
                cell.ndcX = (static_cast<float>(col) + 0.5f) / kCols * 2.0f - 1.0f;
                cell.ndcY = 1.0f - (static_cast<float>(row) + 0.5f) / kRows * 2.0f;
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
        glReadBuffer(static_cast<GLenum>(readBuf));

        for (int pass = 0; pass < 2; ++pass)
        {
            const bool greenPass = pass == 0;
            for (int pick = 0; pick < 3; ++pick)
            {
                Cell *best = nullptr;
                for (int i = 0; i < kCols * kRows; ++i)
                {
                    const float score = greenPass ? cells[i].green : cells[i].red;
                    if (score > 0.03f && (!best || score > (greenPass ? best->green : best->red)))
                        best = &cells[i];
                }
                if (!best)
                    break;
                diag::Log("lightprobe", "%s hot#%d score=%.3f ndc=(%.2f,%.2f)",
                          greenPass ? "green" : "red", pick,
                          greenPass ? best->green : best->red, best->ndcX, best->ndcY);
                if (greenPass) best->green = -1.0f; else best->red = -1.0f;
            }
        }

        // Compact spatial map: one char per cell, green/red dominance coded
        // as digits (0-9 = strength), '.' below threshold. Rows print
        // top-to-bottom so the layout matches the screen.
        static long long dumpFrame = 0;
        if (frame - dumpFrame >= 360)
        {
            dumpFrame = frame;
            for (int row = 0; row < kRows; ++row)
            {
                char line[kCols + 1] = {};
                for (int col = 0; col < kCols; ++col)
                {
                    const Cell &cell = cells[row * kCols + col];
                    float score = std::max(cell.green, cell.red);
                    char code = '.';
                    if (score > 0.03f)
                        code = char('0' + static_cast<int>(std::min(score * 10.0f, 9.0f)));
                    line[col] = code;
                }
                line[kCols] = 0;
                diag::Log("lightprobe", "map row%d |%s|", row, line);
            }
        }

        // Where SHOULD the nearest light appear? Project it with the same
        // view-projection the billboards use.
        if (glcap::HasViewProjection())
        {
            const float *matrix = glcap::ViewProjectionMatrix();
            const bool transposed = glcap::MatrixTransposed();
            std::vector<std::shared_ptr<Light>> lights;
            {
                std::lock_guard<std::mutex> lock(RegistryMutex());
                lights = Registry();
            }
            glm::vec3 position(0.0f);
            bool has = false;
            float bestDistance = 0.0f;
            const float *camera = glcap::ViewPosition();
            for (const auto &light : lights)
            {
                if (!light)
                    continue;
                glm::vec3 candidate(light->x, light->y, light->z);
                if (light->follow &&
                    light->humanIndex >= 0 &&
                    light->humanIndex < static_cast<int>(structs::Human::VanillaCount) &&
                    addresses::Humans[light->humanIndex].isActive.b1)
                {
                    const auto &human = addresses::Humans[light->humanIndex];
                    candidate = glm::vec3(
                        human.position.x + light->offsetX,
                        human.position.y + light->offsetY,
                        human.position.z + light->offsetZ);
                }
                const float dx = candidate.x - camera[0];
                const float dy = candidate.y - camera[1];
                const float dz = candidate.z - camera[2];
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (!has || distanceSquared < bestDistance)
                {
                    has = true;
                    bestDistance = distanceSquared;
                    position = candidate;
                }
            }
            if (has)
            {
                const float point[3] = {position.x, position.y, position.z};
                float clip[4];
                for (int r = 0; r < 4; ++r)
                    clip[r] = transposed
                        ? matrix[r * 4 + 0] * point[0] + matrix[r * 4 + 1] * point[1] + matrix[r * 4 + 2] * point[2] + matrix[r * 4 + 3]
                        : matrix[0 * 4 + r] * point[0] + matrix[1 * 4 + r] * point[1] + matrix[2 * 4 + r] * point[2] + matrix[3 * 4 + r];
                if (clip[3] > 0.05f)
                    diag::Log("lightprobe", "light world=(%.1f,%.1f,%.1f) expected ndc=(%.2f,%.2f) w=%.1f",
                              point[0], point[1], point[2], clip[0] / clip[3], clip[1] / clip[3], clip[3]);
                else
                    diag::Log("lightprobe", "light world=(%.1f,%.1f,%.1f) BEHIND CAMERA", point[0], point[1], point[2]);
            }
        }
#endif
    }

        void Shutdown()
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            Registry().clear();
            Renderer &renderer = GetRenderer();
            if (renderer.vao) { glDeleteVertexArrays(1, &renderer.vao); renderer.vao = 0; }
            if (renderer.vbo) { glDeleteBuffers(1, &renderer.vbo); renderer.vbo = 0; }
            if (renderer.program) { glDeleteProgram(renderer.program); renderer.program = 0; }
            renderer.ready = false;
        }
    }
}
