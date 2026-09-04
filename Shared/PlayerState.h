#pragma once

#include <cstdint>
#include <vector>

namespace SkyrimMP
{
    struct Vec3
    {
        float x{};
        float y{};
        float z{};
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
        std::vector<std::uint32_t> equippedFormIds;
    };
}
