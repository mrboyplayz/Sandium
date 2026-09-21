#include "Footsteps.hpp"

#include "Addresses.hpp"
#include "api/Logging.hpp"
#include "api/Sound.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>

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

        using Clock = std::chrono::steady_clock;
        struct HumanStepState
        {
            bool active = false;
            bool grounded = false;
            structs::CVector3 previous{};
            float distance = 0.0f;
            Clock::time_point lastFrame{};
            Clock::time_point lastStep{};
            std::uint32_t sequence = 0;
        };

        std::array<HumanStepState, structs::Human::VanillaCount> states{};
        std::array<std::array<std::shared_ptr<api::Sound>, SAMPLE_COUNT>,
                   static_cast<std::size_t>(Material::Count)> sounds{};
        bool loadAttempted = false;
        bool soundsReady = false;

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
            int materialID = -1;
            if (hit.areaID >= 0 && hit.areaID < 4 && addresses::SurfaceMaterialLookupFunc.ptr)
            {
                materialID = addresses::SurfaceMaterialLookupFunc(
                    static_cast<unsigned int>(hit.areaID), static_cast<unsigned int>(hit.blockX),
                    static_cast<unsigned int>(hit.blockY), static_cast<unsigned int>(hit.blockZ),
                    hit.faceMaterialSlot);
            }
            else
            {
                materialID = LooseObjectMaterialUnder(position);
            }
            const Material material = Classify(LowerTextureName(materialID));
            *addresses::LineIntersectResult.ptr = savedResult;
            return material;
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
                        sounds[material][sample] = api::Sound::Load3D(path, soundID++, 1.8f);
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
            const float volume = std::clamp(0.58f + speed * 0.080f, 0.66f, 1.18f);
            sounds[static_cast<std::size_t>(material)][sample]->PlayOneShot3D(
                position.x, position.y - 0.8f, position.z, volume, pitch);
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

            const bool grounded = human.isOnGround && human.isStanding && human.vehicleID < 0 &&
                                  human.movementStateID < 5;
            if (!state.active)
            {
                state.active = true;
                state.grounded = grounded;
                state.previous = human.position;
                state.distance = 0.0f;
                state.lastFrame = now;
                state.lastStep = now;
                continue;
            }

            const float dx = human.position.x - state.previous.x;
            const float dz = human.position.z - state.previous.z;
            const float travel = std::sqrt(dx * dx + dz * dz);
            const float elapsed = std::chrono::duration<float>(now - state.lastFrame).count();
            state.previous = human.position;
            state.lastFrame = now;

            if (!grounded || travel > 4.0f || elapsed <= 0.0f || elapsed > 0.5f)
            {
                state.distance = 0.0f;
                state.grounded = grounded;
                continue;
            }

            const float speed = travel / elapsed;
            if (speed < 0.35f)
                continue;
            state.distance += travel;
            const float stride = speed > 5.0f ? 1.42f : 1.02f;
            const float sinceStep = std::chrono::duration<float>(now - state.lastStep).count();
            const float minimumInterval = speed > 5.0f ? 0.15f : 0.22f;
            if (state.distance >= stride && sinceStep >= minimumInterval)
            {
                state.distance = std::fmod(state.distance, stride);
                state.lastStep = now;
                PlayStep(humanID, human.position, speed, state);
            }
            state.grounded = true;
        }
#endif
    }
}
