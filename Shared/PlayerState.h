#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimMP
{
    enum PlayerActionFlag : std::uint16_t
    {
        kWeaponDrawn = 1u << 0,
        kMoving = 1u << 1,
        kRunning = 1u << 2,
        kSprinting = 1u << 3,
        kSneaking = 1u << 4,
        kJumping = 1u << 5,
        kAttacking = 1u << 6,
        kBlocking = 1u << 7,
        kCasting = 1u << 8,
        kKnownPlayerActionFlags = (1u << 9) - 1
    };

    struct Vec3
    {
        float x{};
        float y{};
        float z{};
    };

    // Appearance uses runtime FormIDs on the client. The network layer converts
    // stable forms to canonical record keys before transmission.
    struct PlayerAppearance
    {
        std::uint64_t revision{};
        bool valid{};
        std::string displayName;
        std::uint32_t raceFormId{};
        std::uint8_t sex{};
        float weight{};
        std::uint32_t hairColorFormId{};
        std::uint32_t faceDetailsFormId{};
        std::uint32_t bodyTintColor{};
        std::vector<std::uint32_t> headPartFormIds;
        std::array<float, 19> faceMorphs{};
        std::array<std::int32_t, 4> faceParts{};
    };

    struct PlayerState
    {
        std::uint64_t characterId{};
        std::uint32_t formId{};
        std::uint32_t cellFormId{};
        std::uint32_t worldspaceFormId{};

        Vec3 position{};
        Vec3 rotation{};

        float health{};
        float magicka{};
        float stamina{};
        bool dead{};
        bool inCombat{};
        bool hasActorState{};
        bool hasStatusState{};
        bool hasEquipmentState{};
        std::uint16_t actionFlags{};
        std::vector<std::uint32_t> equippedFormIds;
        PlayerAppearance appearance;
    };
}
