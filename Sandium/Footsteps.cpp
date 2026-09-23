#include "Footsteps.hpp"

#include "Addresses.hpp"
#include "api/Http.hpp"
#include "api/Logging.hpp"
#include "api/Sound.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace footsteps
{
    namespace
    {
        enum class Material : std::size_t
        {
            Concrete,
            Dirt,
            Grass,
            Gravel,
            Metal,
            MetalGrate,
            Sand,
            Slosh,
            Tile,
            Wood,
            Count
        };

        constexpr std::array<const char *, static_cast<std::size_t>(Material::Count)> MATERIAL_NAMES = {
            "concrete", "dirt", "grass", "gravel", "metal", "metalgrate",
            "sand", "slosh", "tile", "wood"};
        constexpr std::size_t SAMPLE_COUNT = 4;
        constexpr unsigned int FIRST_SOUND_ID = 4000;
        constexpr std::size_t TEXTURE_ENTRY_STRIDE = 184;
        constexpr float AUDIBLE_RADIUS = 26.0f;

        using Clock = std::chrono::steady_clock;
        struct HumanStepState
        {
            bool active = false;
            bool grounded = false;
            bool footDown[2] = {};
            bool footSeen[2] = {};
            structs::CVector3 previousFoot[2]{};
            float previousFootRelativeY[2] = {};
            float previousFootVelocityY[2] = {};
            structs::CVector3 previous{};
            Clock::time_point lastFrame{};
            Clock::time_point lastStep{};
            Clock::time_point lastFootStep[2]{};
            std::uint32_t sequence = 0;
        };

        std::array<HumanStepState, structs::Human::VanillaCount> states{};
        std::array<std::array<std::shared_ptr<api::Sound>, SAMPLE_COUNT>,
                   static_cast<std::size_t>(Material::Count)> sounds{};
        bool loadAttempted = false;
        bool soundsReady = false;

        void ResetState(HumanStepState &state, const structs::CVector3 &position,
                        const Clock::time_point &now, bool grounded);

        struct StepEvent
        {
            long long id;
            float x, y, z, speed;
            unsigned int sample;
        };
        std::mutex eventsMutex;
        std::vector<StepEvent> pendingEvents;
        std::atomic<bool> eventsStarted{false};
        std::atomic<long long> lastEventID{0};
        bool eventBaselineReady = false;

        void EventThreadProc()
        {
            for (;;)
            {
                const std::string body = http::Get("185.227.111.150", 80,
                                                   "/stream/footsteps.txt", 2500,
                                                   "SANDIUM_MASTER");
                if (!body.empty())
                {
                    std::vector<StepEvent> received;
                    std::size_t start = 0;
                    while (start < body.size())
                    {
                        const std::size_t end = body.find('\n', start);
                        const std::string line = body.substr(start, end - start);
                        long long id = 0;
                        char human[96] = {};
                        StepEvent event{};
                        if (std::sscanf(line.c_str(), "step %lld %95s %f %f %f %f %u",
                                        &id, human, &event.x, &event.y, &event.z,
                                        &event.speed, &event.sample) == 7)
                        {
                            event.id = id;
                            if (id > lastEventID.load())
                                received.push_back(event);
                        }
                        if (end == std::string::npos)
                            break;
                        start = end + 1;
                    }
                    if (!received.empty())
                    {
                        lastEventID = received.back().id;
                        // The first poll establishes the current tail. Never
                        // replay the complete server history after joining.
                        if (eventBaselineReady)
                        {
                            const std::lock_guard<std::mutex> lock(eventsMutex);
                            pendingEvents.insert(pendingEvents.end(), received.begin(), received.end());
                        }
                        else
                            eventBaselineReady = true;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(75));
            }
        }

        void StartEvents()
        {
            if (!eventsStarted.exchange(true))
                std::thread(EventThreadProc).detach();
        }

        void PlayServerEvents()
        {
            std::vector<StepEvent> events;
            {
                const std::lock_guard<std::mutex> lock(eventsMutex);
                events.swap(pendingEvents);
            }
            for (const StepEvent &event : events)
            {
                // The VPS only tells us that and where a step happened. Each
                // client owns audibility: use its own local player position so
                // distance cannot be wrong because of server/client camera
                // state or another player's viewpoint.
                const structs::Human *listener = nullptr;
                for (std::size_t humanID = 0; humanID < structs::Human::VanillaCount; ++humanID)
                    if (addresses::Humans[humanID].isActive.b1)
                    {
                        listener = &addresses::Humans[humanID];
                        break;
                    }
                if (!listener)
                    continue;
                const float dx = event.x - listener->position.x;
                const float dy = event.y - listener->position.y;
                const float dz = event.z - listener->position.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance >= AUDIBLE_RADIUS)
                    continue;
                const float distanceGain = 1.0f - distance / AUDIBLE_RADIUS;
                const std::size_t sample = event.sample % SAMPLE_COUNT;
                const float volume = std::clamp(0.86f + event.speed * 0.09f, 0.95f, 1.55f) *
                                     distanceGain * distanceGain;
                sounds[static_cast<std::size_t>(Material::Concrete)][sample]->PlayOneShot3D(
                    event.x, event.y + 0.08f, event.z, volume,
                    0.94f + static_cast<float>(sample) * 0.02f);
            }
        }

        std::string LowerTextureName(int materialID)
        {
            if (materialID < 0 || !addresses::LevelTextureCount.ptr ||
                !addresses::LevelTextureNames.ptr)
                return {};
            const int count = *addresses::LevelTextureCount.ptr;
            if (count <= 0 || count > 4096 || materialID >= count)
                return {};
            const char *name = addresses::LevelTextureNames.ptr +
                               static_cast<std::size_t>(materialID) * TEXTURE_ENTRY_STRIDE;
            std::size_t length = 0;
            while (length < 64 && name[length])
                ++length;
            std::string result(name, length);
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        bool Has(const std::string &name, const char *part)
        {
            return name.find(part) != std::string::npos;
        }

        Material Classify(const std::string &texture)
        {
            if (Has(texture, "grass"))
                return Material::Grass;
            if (Has(texture, "gravel") || Has(texture, "rock") || Has(texture, "stone"))
                return Material::Gravel;
            if (Has(texture, "dirt") || Has(texture, "mud") || Has(texture, "soil"))
                return Material::Dirt;
            if (Has(texture, "sand"))
                return Material::Sand;
            if (Has(texture, "water") || Has(texture, "slosh"))
                return Material::Slosh;
            if (Has(texture, "grate") || Has(texture, "chain"))
                return Material::MetalGrate;
            if (Has(texture, "metal") || Has(texture, "steel") || Has(texture, "roof"))
                return Material::Metal;
            if (Has(texture, "wood") || Has(texture, "panel"))
                return Material::Wood;
            if (Has(texture, "tile") || Has(texture, "carpet") || Has(texture, "plaster"))
                return Material::Tile;
            // Asphalt, brick, painted walls and the unmapped terrain plane all
            // use HL2's concrete family, which is its closest hard-surface set.
            return Material::Concrete;
        }

        template <typename T>
        T ReadAt(const std::uint8_t *base, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, base + offset, sizeof(value));
            return value;
        }

        structs::CVector3 ObjectVertex(const std::uint8_t *object, const std::uint8_t *model,
                                       int vertexID)
        {
            const std::size_t vertex = 8 + static_cast<std::size_t>(vertexID) * 12;
            const float x = ReadAt<float>(model, vertex + 0);
            const float y = ReadAt<float>(model, vertex + 4);
            const float z = ReadAt<float>(model, vertex + 8);
            return structs::CVector3(
                ReadAt<float>(object, 8) + ReadAt<float>(object, 32) * x +
                    ReadAt<float>(object, 44) * y + ReadAt<float>(object, 56) * z,
                ReadAt<float>(object, 12) + ReadAt<float>(object, 36) * x +
                    ReadAt<float>(object, 48) * y + ReadAt<float>(object, 60) * z,
                ReadAt<float>(object, 16) + ReadAt<float>(object, 40) * x +
                    ReadAt<float>(object, 52) * y + ReadAt<float>(object, 64) * z);
        }

        bool DownwardTriangleHit(const structs::CVector3 &origin, const structs::CVector3 &a,
                                 const structs::CVector3 &b, const structs::CVector3 &c,
                                 float &distance)
        {
            // Moller-Trumbore specialized to a vertical, downward unit ray.
            const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
            const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
            const float px = -e2z, py = 0.0f, pz = e2x;
            const float determinant = e1x * px + e1y * py + e1z * pz;
            if (std::fabs(determinant) < 0.00001f)
                return false;
            const float inverse = 1.0f / determinant;
            const float tx = origin.x - a.x, ty = origin.y - a.y, tz = origin.z - a.z;
            const float u = (tx * px + ty * py + tz * pz) * inverse;
            if (u < -0.001f || u > 1.001f)
                return false;
            const float qx = ty * e1z - tz * e1y;
            const float qy = tz * e1x - tx * e1z;
            const float qz = tx * e1y - ty * e1x;
            const float v = -qy * inverse;
            if (v < -0.001f || u + v > 1.001f)
                return false;
            distance = (e2x * qx + e2y * qy + e2z * qz) * inverse;
            return distance >= 0.0f && distance <= 3.0f;
        }

        int LooseObjectMaterialUnder(const structs::CVector3 &position)
        {
            if (!addresses::Base.ptr)
                return -1;
            constexpr std::size_t OBJECTS_RVA = 0x6796F380;
            constexpr std::size_t OBJECT_STRIDE = 84;
            constexpr std::size_t MODEL_DATA_RVA = 0x675FE780;
            constexpr std::size_t MODEL_STRIDE = 9240;
            constexpr std::size_t FACE_COUNT = 0xC08;
            constexpr std::size_t FACE_DATA = 0xC0C;
            constexpr std::size_t FACE_STRIDE = 24;
            const auto *image = reinterpret_cast<const std::uint8_t *>(addresses::Base.ptr);
            const structs::CVector3 origin(position.x, position.y + 0.35f, position.z);
            float nearest = std::numeric_limits<float>::max();
            int nearestMaterial = -1;

            for (int objectID = 0; objectID < 128; ++objectID)
            {
                const auto *object = image + OBJECTS_RVA + objectID * OBJECT_STRIDE;
                if (ReadAt<int>(object, 0) == 0)
                    continue;
                const int modelID = ReadAt<int>(object, 4);
                if (modelID < 0 || modelID >= 256)
                    continue;
                const auto *model = image + MODEL_DATA_RVA + modelID * MODEL_STRIDE;
                const float radius = ReadAt<float>(model, 0);
                const float dx = position.x - ReadAt<float>(object, 8);
                const float dz = position.z - ReadAt<float>(object, 16);
                if (radius <= 0.0f || dx * dx + dz * dz > (radius + 1.0f) * (radius + 1.0f))
                    continue;
                const int faceCount = ReadAt<int>(model, FACE_COUNT);
                if (faceCount <= 0 || faceCount > 256)
                    continue;

                for (int faceID = 0; faceID < faceCount; ++faceID)
                {
                    const auto *face = model + FACE_DATA + faceID * FACE_STRIDE;
                    const int vertices = ReadAt<int>(face, 0);
                    const int aID = ReadAt<int>(face, 4);
                    const int bID = ReadAt<int>(face, 8);
                    const int cID = ReadAt<int>(face, 12);
                    if ((vertices != 3 && vertices != 4) || aID < 0 || aID >= 256 ||
                        bID < 0 || bID >= 256 || cID < 0 || cID >= 256)
                        continue;
                    const auto a = ObjectVertex(object, model, aID);
                    const auto b = ObjectVertex(object, model, bID);
                    const auto c = ObjectVertex(object, model, cID);
                    float distance = 0.0f;
                    bool hit = DownwardTriangleHit(origin, a, b, c, distance);
                    if (!hit && vertices == 4)
                    {
                        const int dID = ReadAt<int>(face, 16);
                        if (dID >= 0 && dID < 256)
                            hit = DownwardTriangleHit(origin, a, c,
                                                      ObjectVertex(object, model, dID), distance);
                    }
                    if (hit && distance < nearest)
                    {
                        nearest = distance;
                        nearestMaterial = ReadAt<int>(face, 20);
                    }
                }
            }
            return nearestMaterial;
        }

        Material MaterialUnder(const structs::CVector3 &position)
        {
            if (!addresses::LineIntersectLevelFunc.ptr || !addresses::LineIntersectResult.ptr)
                return Material::Concrete;

            // The engine exposes one global scratch result shared by all ray
            // callers. Preserve it so a HUD-frame foot probe cannot disturb a
            // vanilla camera, weapon, or selection query.
            const structs::LineIntersectResult savedResult = *addresses::LineIntersectResult.ptr;
            structs::CVector3 start(position.x, position.y + 0.35f, position.z);
            structs::CVector3 end(position.x, position.y - 2.25f, position.z);
            if (!addresses::LineIntersectLevelFunc(&start, &end, 1))
            {
                *addresses::LineIntersectResult.ptr = savedResult;
                return Material::Concrete;
            }

            const structs::LineIntersectResult hit = *addresses::LineIntersectResult.ptr;
            int materialID = hit.materialID;
            if (materialID < 0 && hit.areaID >= 0 && hit.areaID < 4 &&
                addresses::SurfaceMaterialLookupFunc.ptr)
            {
                materialID = addresses::SurfaceMaterialLookupFunc(
                    static_cast<unsigned int>(hit.areaID), static_cast<unsigned int>(hit.blockX),
                    static_cast<unsigned int>(hit.blockY), static_cast<unsigned int>(hit.blockZ),
                    hit.faceMaterialSlot);
            }
            if (materialID < 0)
            {
                materialID = LooseObjectMaterialUnder(position);
            }
            const Material material = Classify(LowerTextureName(materialID));
            *addresses::LineIntersectResult.ptr = savedResult;
            return material;
        }

        bool FootNearGround(const structs::CVector3 &position, float &distance)
        {
            distance = 0.0f;
            if (!addresses::LineIntersectLevelFunc.ptr || !addresses::LineIntersectResult.ptr)
                return true;

            const structs::LineIntersectResult savedResult = *addresses::LineIntersectResult.ptr;
            structs::CVector3 start(position.x, position.y + 0.18f, position.z);
            structs::CVector3 end(position.x, position.y - 0.55f, position.z);
            const bool hit = addresses::LineIntersectLevelFunc(&start, &end, 1) != 0;
            const structs::LineIntersectResult result = *addresses::LineIntersectResult.ptr;
            *addresses::LineIntersectResult.ptr = savedResult;
            if (!hit)
                return false;

            const float dx = position.x - result.position.x;
            const float dy = position.y - result.position.y;
            const float dz = position.z - result.position.z;
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            return distance <= 0.34f;
        }

        bool HumanNearGround(const structs::CVector3 &position)
        {
            if (!addresses::LineIntersectLevelFunc.ptr || !addresses::LineIntersectResult.ptr)
                return true;
            const structs::LineIntersectResult savedResult = *addresses::LineIntersectResult.ptr;
            structs::CVector3 start(position.x, position.y + 0.2f, position.z);
            structs::CVector3 end(position.x, position.y - 2.5f, position.z);
            const bool hit = addresses::LineIntersectLevelFunc(&start, &end, 1) != 0;
            *addresses::LineIntersectResult.ptr = savedResult;
            return hit;
        }

        bool LoadSounds()
        {
            if (loadAttempted)
                return soundsReady;
            loadAttempted = true;
            try
            {
                unsigned int soundID = FIRST_SOUND_ID;
                for (std::size_t material = 0; material < MATERIAL_NAMES.size(); ++material)
                {
                    for (std::size_t sample = 0; sample < SAMPLE_COUNT; ++sample)
                    {
                        const std::string path = std::string("sandium/Assets/footsteps/") +
                            MATERIAL_NAMES[material] + std::to_string(sample + 1) + ".wav";
                        sounds[material][sample] = api::Sound::Load3D(path, soundID++, 4.5f);
                    }
                }
                soundsReady = true;
                api::GetSandiumLogger()->Log("Loaded {} material-aware HL2 footstep samples",
                                             MATERIAL_NAMES.size() * SAMPLE_COUNT);
            }
            catch (const std::exception &error)
            {
                api::GetSandiumLogger()->Log("<red>Footstep audio disabled: {}", error.what());
            }
            return soundsReady;
        }

        void PlayStep(std::size_t humanID, const structs::CVector3 &position, float speed,
                      HumanStepState &state)
        {
            const Material material = MaterialUnder(position);
            const std::uint32_t hash = static_cast<std::uint32_t>(humanID * 1103515245u) +
                                       state.sequence++ * 2654435761u;
            const std::size_t sample = (hash >> 16) % SAMPLE_COUNT;
            const float pitch = 0.94f + static_cast<float>((hash >> 8) & 15u) * 0.008f;
            const float volume = std::clamp(0.86f + speed * 0.090f, 0.95f, 1.55f);
            sounds[static_cast<std::size_t>(material)][sample]->PlayOneShot3D(
                position.x, position.y + 0.08f, position.z, volume, pitch);
        }

        void UpdateClientSteps()
        {
            const Clock::time_point now = Clock::now();
            for (std::size_t humanID = 0; humanID < states.size(); ++humanID)
            {
                const auto &human = addresses::Humans[humanID];
                auto &state = states[humanID];
                if (!human.isActive.b1 || human.vehicleID >= 0)
                {
                    state.active = false;
                    continue;
                }
                if (!state.active)
                {
                    ResetState(state, human.position, now, false);
                    continue;
                }
                const float elapsed = std::chrono::duration<float>(now - state.lastFrame).count();
                const float dx = human.position.x - state.previous.x;
                const float dz = human.position.z - state.previous.z;
                const float travel = std::sqrt(dx * dx + dz * dz);
                state.previous = human.position;
                state.lastFrame = now;
                if (elapsed <= 0.0f || elapsed > 0.5f || travel > 4.0f)
                    continue; // paused frame, join correction, or teleport
                const float speed = travel / elapsed;
                if (speed < 0.35f || !HumanNearGround(human.position))
                    continue;
                const float sinceStep = std::chrono::duration<float>(now - state.lastStep).count();
                const float interval = std::clamp(0.62f - speed * 0.075f, 0.18f, 0.58f);
                if (sinceStep >= interval)
                {
                    state.lastStep = now;
                    PlayStep(humanID, human.position, speed, state);
                }
            }
        }

        void ResetState(HumanStepState &state, const structs::CVector3 &position,
                        const Clock::time_point &now, bool grounded)
        {
            state.active = true;
            state.grounded = grounded;
            state.footDown[0] = false;
            state.footDown[1] = false;
            state.footSeen[0] = false;
            state.footSeen[1] = false;
            state.previousFoot[0] = {};
            state.previousFoot[1] = {};
            state.previousFootRelativeY[0] = 0.0f;
            state.previousFootRelativeY[1] = 0.0f;
            state.previousFootVelocityY[0] = 0.0f;
            state.previousFootVelocityY[1] = 0.0f;
            state.previous = position;
            state.lastFrame = now;
            state.lastStep = now;
            state.lastFootStep[0] = now;
            state.lastFootStep[1] = now;
        }
    }

    void Update()
    {
#if _WIN32
        if (!addresses::IsInGame.ptr || !*addresses::IsInGame || !addresses::Humans.ptr)
        {
            for (auto &state : states)
                state.active = false;
            return;
        }
        if (!LoadSounds())
            return;
        // Immediate local detection: each client checks grounded movement and
        // material under every active player, with no VPS event/poll delay.
        UpdateClientSteps();
        return;

        const Clock::time_point now = Clock::now();
        for (std::size_t humanID = 0; humanID < states.size(); ++humanID)
        {
            const auto &human = addresses::Humans[humanID];
            auto &state = states[humanID];
            if (!human.isActive)
            {
                state.active = false;
                continue;
            }

            // Remote standing state is unreliable.  Keep its replicated
            // ground flag only for the movement-cadence fallback; animated
            // feet use their own raycast below for exact local contact.
            const bool canWalk = human.vehicleID < 0;
            const bool networkGrounded = human.isOnGround && canWalk;
            if (!state.active)
            {
                ResetState(state, human.position, now, networkGrounded);
                continue;
            }

            const float dx = human.position.x - state.previous.x;
            const float dz = human.position.z - state.previous.z;
            const float travel = std::sqrt(dx * dx + dz * dz);
            const float elapsed = std::chrono::duration<float>(now - state.lastFrame).count();
            state.previous = human.position;
            state.lastFrame = now;

            if (!canWalk || travel > 4.0f || elapsed <= 0.0f || elapsed > 0.5f)
            {
                state.grounded = networkGrounded;
                state.footDown[0] = false;
                state.footDown[1] = false;
                state.footSeen[0] = false;
                state.footSeen[1] = false;
                continue;
            }

            const float speed = travel / elapsed;
            if (speed < 0.35f)
            {
                state.footDown[0] = false;
                state.footDown[1] = false;
                continue;
            }

            constexpr int FOOT_BONES[2] = {12, 15};
            bool playedFromFoot = false;
            for (int foot = 0; foot < 2; ++foot)
            {
                const structs::CVector3 footPosition = human.bones[FOOT_BONES[foot]].position;
                if (!std::isfinite(footPosition.x) || !std::isfinite(footPosition.y) ||
                    !std::isfinite(footPosition.z))
                {
                    state.footDown[foot] = false;
                    state.footSeen[foot] = false;
                    continue;
                }

                if (!state.footSeen[foot])
                {
                    state.footSeen[foot] = true;
                    state.previousFoot[foot] = footPosition;
                    state.previousFootRelativeY[foot] = footPosition.y - human.position.y;
                    state.previousFootVelocityY[foot] = 0.0f;
                    continue;
                }

                const float footTravelX = footPosition.x - state.previousFoot[foot].x;
                const float footTravelY = footPosition.y - state.previousFoot[foot].y;
                const float footTravelZ = footPosition.z - state.previousFoot[foot].z;
                if (footTravelX * footTravelX + footTravelY * footTravelY +
                    footTravelZ * footTravelZ > 16.0f)
                {
                    state.footDown[foot] = false;
                    state.footSeen[foot] = false;
                    continue;
                }

                float groundDistance = 0.0f;
                const bool rayDown = FootNearGround(footPosition, groundDistance);
                const float relativeY = footPosition.y - human.position.y;
                const float velocityY = (relativeY - state.previousFootRelativeY[foot]) / elapsed;
                const bool localLowPoint = state.previousFootVelocityY[foot] < -0.03f &&
                                           velocityY >= -0.005f;
                const bool lowEnough = relativeY <= state.previousFootRelativeY[1 - foot] + 0.18f;
                const bool down = rayDown ||
                                  (networkGrounded && localLowPoint && lowEnough);
                const float sinceFoot = std::chrono::duration<float>(
                    now - state.lastFootStep[foot]).count();
                if (down && !state.footDown[foot] && sinceFoot >= 0.18f)
                {
                    state.lastFootStep[foot] = now;
                    state.lastStep = now;
                    PlayStep(humanID, footPosition, speed, state);
                    playedFromFoot = true;
                }
                state.footDown[foot] = down;
                state.previousFoot[foot] = footPosition;
                state.previousFootRelativeY[foot] = relativeY;
                state.previousFootVelocityY[foot] = velocityY;
            }

            // Remote players do not always receive live foot-bone animation
            // from the server.  Their replicated root position is reliable,
            // though, so retain the detailed contact detection where it is
            // available and fall back to a speed-based walking cadence when
            // it is not.  This is what makes other players' steps audible.
            if (!playedFromFoot && networkGrounded)
            {
                const float sinceLastStep = std::chrono::duration<float>(
                    now - state.lastStep).count();
                const float stepInterval = std::clamp(0.62f - speed * 0.075f,
                                                      0.25f, 0.58f);
                if (sinceLastStep >= stepInterval)
                {
                    state.lastStep = now;
                    PlayStep(humanID, human.position, speed, state);
                }
            }
            state.grounded = networkGrounded;
        }
#endif
    }
}
