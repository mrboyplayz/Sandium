#include "BetterCrashes.hpp"

#include "Addresses.hpp"
#include "api/Logging.hpp"
#include "structs/Human.hpp"
#include "structs/Vehicle.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace bettercrashes
{
    namespace
    {
        struct VehicleHistory
        {
            structs::CVector3 position;
            bool valid = false;
        };

        std::array<VehicleHistory, structs::Vehicle::VanillaCount> history;
        std::array<unsigned char, structs::Human::VanillaCount> cooldown{};
        std::array<int, structs::Human::VanillaCount> lastSeatedVehicle = []
        {
            std::array<int, structs::Human::VanillaCount> values{};
            values.fill(-1);
            return values;
        }();
        std::array<unsigned char, structs::Human::VanillaCount> exitGrace{};
        std::array<unsigned char, structs::Human::VanillaCount> exitProtection{};
        std::array<bool, structs::Human::VanillaCount> wasSeated{};

        struct Vitality
        {
            int damage = 0;
            int health = 0;
            int bloodLevel = 0;
            bool isBleeding = false;
            int torsoHealth = 0;
            int headHealth = 0;
            int leftArmHealth = 0;
            int rightArmHealth = 0;
            int leftLegHealth = 0;
            int rightLegHealth = 0;
        };
        std::array<Vitality, structs::Human::VanillaCount> seatedVitality{};

        void RememberVitality(std::size_t id, const structs::Human &human)
        {
            seatedVitality[id] = {human.damage, human.health, human.bloodLevel,
                human.isBleeding.b1, human.torsoHealth, human.headHealth,
                human.leftArmHealth, human.rightArmHealth,
                human.leftLegHealth, human.rightLegHealth};
        }

        void ProtectVehicleExit(std::size_t id, structs::Human &human)
        {
            const auto &vitality = seatedVitality[id];
            human.damage = std::min(human.damage, vitality.damage);
            human.health = std::max(human.health, vitality.health);
            human.bloodLevel = std::max(human.bloodLevel, vitality.bloodLevel);
            human.isBleeding.b1 = human.isBleeding.b1 && vitality.isBleeding;
            human.torsoHealth = std::max(human.torsoHealth, vitality.torsoHealth);
            human.headHealth = std::max(human.headHealth, vitality.headHealth);
            human.leftArmHealth = std::max(human.leftArmHealth, vitality.leftArmHealth);
            human.rightArmHealth = std::max(human.rightArmHealth, vitality.rightArmHealth);
            human.leftLegHealth = std::max(human.leftLegHealth, vitality.leftLegHealth);
            human.rightLegHealth = std::max(human.rightLegHealth, vitality.rightLegHealth);
        }

        float HorizontalDistanceSquared(const structs::CVector3 &a, const structs::CVector3 &b)
        {
            const float dx = a.x - b.x;
            const float dz = a.z - b.z;
            return dx * dx + dz * dz;
        }

        float Dot(const structs::CVector3 &a, const structs::CVector3 &b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        structs::CVector3 Scale(const structs::CVector3 &v, float scale)
        {
            return {v.x * scale, v.y * scale, v.z * scale};
        }

        void ResolveCarBody(structs::Human &human, const structs::Vehicle &vehicle)
        {
            // Approximate the solid passenger-car body with an oriented box. The
            // game's discrete collision can miss this box at high speed; resolving
            // every penetrating bone gives the ragdoll a persistent contact surface.
            constexpr float halfWidth = 1.25f;
            constexpr float halfHeight = 1.05f;
            constexpr float halfLength = 2.75f;
            const auto &right = vehicle.orientation.right;
            const auto &up = vehicle.orientation.up;
            const auto &back = vehicle.orientation.back;

            for (auto &bone : human.bones)
            {
                const structs::CVector3 relative(bone.position.x - vehicle.position.x,
                                                 bone.position.y - vehicle.position.y,
                                                 bone.position.z - vehicle.position.z);
                const float localX = Dot(relative, right);
                const float localY = Dot(relative, up);
                const float localZ = Dot(relative, back);
                if (std::abs(localX) > halfWidth || std::abs(localY) > halfHeight ||
                    std::abs(localZ) > halfLength)
                    continue;

                const float pushX = halfWidth - std::abs(localX);
                const float pushY = halfHeight - std::abs(localY);
                const float pushZ = halfLength - std::abs(localZ);
                structs::CVector3 normal;
                float depth = 0.0f;
                if (pushY <= pushX && pushY <= pushZ)
                {
                    // Prefer the roof over pushing a struck ragdoll underneath.
                    normal = localY >= -0.2f ? up : Scale(up, -1.0f);
                    depth = pushY;
                }
                else if (pushX <= pushZ)
                {
                    normal = Scale(right, localX >= 0.0f ? 1.0f : -1.0f);
                    depth = pushX;
                }
                else
                {
                    normal = Scale(back, localZ >= 0.0f ? 1.0f : -1.0f);
                    depth = pushZ;
                }

                const structs::CVector3 correction = Scale(normal, depth + 0.08f);
                bone.position.x += correction.x;
                bone.position.y += correction.y;
                bone.position.z += correction.z;
                bone.alternativePosition = bone.position;

                structs::CVector3 relativeVelocity(bone.velocity.x - vehicle.velocity.x,
                                                   bone.velocity.y - vehicle.velocity.y,
                                                   bone.velocity.z - vehicle.velocity.z);
                const float inwardSpeed = Dot(relativeVelocity, normal);
                if (inwardSpeed < 0.0f)
                {
                    // Remove inward motion and retain a small bounce from the car.
                    const float response = -inwardSpeed * 1.08f;
                    bone.velocity.x += normal.x * response;
                    bone.velocity.y += normal.y * response;
                    bone.velocity.z += normal.z * response;
                }
            }
        }

        void ApplyImpact(structs::Human &human, const structs::Vehicle &vehicle,
                         const structs::CVector3 &reference, const structs::CVector3 &closest,
                         float dirX, float dirZ, float speed)
        {
            // Put the ragdoll beyond the leading bumper and give it enough of the
            // car's momentum to remain ahead during the next physics step.
            structs::CVector3 target = closest;
            target.x += dirX * 2.15f;
            target.z += dirZ * 2.15f;
            target.y = std::max(human.position.y, vehicle.position.y + 0.65f);

            structs::CVector3 correction(target.x - reference.x,
                                         target.y - reference.y,
                                         target.z - reference.z);
            const float correctionLength = correction.Length();
            if (correctionLength > 4.0f)
            {
                const float scale = 4.0f / correctionLength;
                correction.x *= scale;
                correction.y *= scale;
                correction.z *= scale;
            }

            human.isPhysicsEnabled = true;
            human.isStanding = false;
            human.position.x += correction.x;
            human.position.y += correction.y;
            human.position.z += correction.z;
            human.alternativePosition = human.position;

            const float launch = std::clamp(speed * 0.72f, 0.35f, 2.8f);
            const float lift = std::clamp(speed * 0.16f, 0.16f, 0.8f);
            for (auto &bone : human.bones)
            {
                bone.position.x += correction.x;
                bone.position.y += correction.y;
                bone.position.z += correction.z;
                bone.alternativePosition = bone.position;
                bone.velocity.x = dirX * launch;
                bone.velocity.y = lift;
                bone.velocity.z = dirZ * launch;
            }
        }

        void ApplyLethalCrashDamage(structs::Human &human)
        {
            // Mark every vital channel before the normal logic tick. This lets the
            // death/ragdoll transition begin at swept contact instead of one frame
            // later, when the vehicle has already overlapped the standing body.
            human.damage = std::max(human.damage, 1000);
            human.health = 0;
            human.bloodLevel = 0;
            human.isBleeding = true;
            human.torsoHealth = 0;
            human.headHealth = 0;
            human.leftArmHealth = 0;
            human.rightArmHealth = 0;
            human.leftLegHealth = 0;
            human.rightLegHealth = 0;
        }
    }

    void Update()
    {
        if (!addresses::Vehicles.ptr || !addresses::Humans.ptr || !*addresses::IsInGame)
            return;

        for (auto &frames : cooldown)
            if (frames) --frames;

        for (std::size_t humanID = 0; humanID < structs::Human::VanillaCount; ++humanID)
        {
            const auto &human = addresses::Humans[humanID];
            if (!human.isActive.b1)
            {
                lastSeatedVehicle[humanID] = -1;
                exitGrace[humanID] = 0;
                exitProtection[humanID] = 0;
                wasSeated[humanID] = false;
            }
            else if (human.vehicleSeatID >= 0)
            {
                lastSeatedVehicle[humanID] = human.vehicleID;
                exitGrace[humanID] = 90;
                exitProtection[humanID] = 0;
                wasSeated[humanID] = true;
                RememberVitality(humanID, human);

                // bailing out: bleed the car's speed off while the exit key is
                // held, so the vanilla exit impact cannot exceed what a human
                // survives. (Q = drop/exit key per the input flag table.)
                structs::Vehicle &vehicle = addresses::Vehicles[human.vehicleID];
                if (human.vehicleID >= 0 && (human.inputFlags & 32) &&
                    std::abs(vehicle.velocity.x) + std::abs(vehicle.velocity.y) + std::abs(vehicle.velocity.z) > 0.5f)
                {
                    vehicle.velocity.x *= 0.82f;
                    vehicle.velocity.y *= 0.82f;
                    vehicle.velocity.z *= 0.82f;
                }
            }
            else
            {
                if (wasSeated[humanID])
                {
                    // Vanilla applies the car's speed as impact damage during the
                    // first few frames outside the seat. Preserve the occupant's
                    // pre-exit vitality until that transition has completed.
                    exitProtection[humanID] = 300;
                    wasSeated[humanID] = false;
                }
                if (exitProtection[humanID])
                {
                    ProtectVehicleExit(humanID, addresses::Humans[humanID]);
                    --exitProtection[humanID];
                }
                if (exitGrace[humanID]) --exitGrace[humanID];
            }
        }

        for (std::size_t vehicleID = 0; vehicleID < structs::Vehicle::VanillaCount; ++vehicleID)
        {
            const auto &vehicle = addresses::Vehicles[vehicleID];
            auto &previous = history[vehicleID];
            if (!vehicle.isActive.b1)
            {
                previous.valid = false;
                continue;
            }

            const structs::CVector3 current = vehicle.position;
            if (!previous.valid)
            {
                previous.position = current;
                previous.valid = true;
                continue;
            }

            // Maintain solid contact even on frames where the car barely moves.
            for (std::size_t humanID = 0; humanID < structs::Human::VanillaCount; ++humanID)
            {
                auto &human = addresses::Humans[humanID];
                const bool ownVehicle = human.vehicleID == static_cast<int>(vehicleID) ||
                                        lastSeatedVehicle[humanID] == static_cast<int>(vehicleID);
                if (!human.isActive.b1 ||
                    (human.vehicleSeatID >= 0 && ownVehicle) ||
                    (exitGrace[humanID] && ownVehicle))
                    continue;
                if (exitProtection[humanID])
                {
                    // just exited a moving car: keep vanilla from flinging the
                    // fresh occupant around while the protection window runs
                    ProtectVehicleExit(humanID, human);
                    continue;
                }
                ResolveCarBody(human, vehicle);
            }

            const float moveX = current.x - previous.position.x;
            const float moveZ = current.z - previous.position.z;
            const float travelSquared = moveX * moveX + moveZ * moveZ;
            const float travel = std::sqrt(travelSquared);
            if (travel < 0.28f || travel > 12.0f)
            {
                previous.position = current;
                continue;
            }

            const float dirX = moveX / travel;
            const float dirZ = moveZ / travel;
            for (std::size_t humanID = 0; humanID < structs::Human::VanillaCount; ++humanID)
            {
                auto &human = addresses::Humans[humanID];
                const bool ownVehicle = human.vehicleID == static_cast<int>(vehicleID) ||
                                        lastSeatedVehicle[humanID] == static_cast<int>(vehicleID);
                if (!human.isActive.b1 || cooldown[humanID] ||
                    (human.vehicleSeatID >= 0 && ownVehicle) ||
                    (exitGrace[humanID] && ownVehicle) ||
                    exitProtection[humanID])
                    continue;

                structs::CVector3 hitReference = human.position;
                structs::CVector3 hitClosest{};
                float bestDistance = 3.25f * 3.25f;
                bool hit = false;
                const auto testPoint = [&](const structs::CVector3 &point)
                {
                    if (std::abs(point.y - current.y) > 2.8f) return;
                    const float relX = point.x - previous.position.x;
                    const float relZ = point.z - previous.position.z;
                    const float t = std::clamp((relX * moveX + relZ * moveZ) / travelSquared, 0.0f, 1.0f);
                    structs::CVector3 closest(previous.position.x + moveX * t,
                                              previous.position.y + (current.y - previous.position.y) * t,
                                              previous.position.z + moveZ * t);
                    const float distance = HorizontalDistanceSquared(point, closest);
                    if (distance <= bestDistance)
                    {
                        bestDistance = distance;
                        hitReference = point;
                        hitClosest = closest;
                        hit = true;
                    }
                };
                testPoint(human.position);
                for (const auto &bone : human.bones) testPoint(bone.position);
                if (!hit)
                    continue;

                ApplyImpact(human, vehicle, hitReference, hitClosest, dirX, dirZ, travel);
                if (travel >= 0.48f)
                    ApplyLethalCrashDamage(human);
                cooldown[humanID] = 4;
                api::GetSandiumLogger()->Log("Better crash: vehicle {} hit human {} (travel {:.2f}, distance {:.2f})",
                                             vehicleID, humanID, travel, std::sqrt(bestDistance));
            }
            previous.position = current;
        }
    }
}
