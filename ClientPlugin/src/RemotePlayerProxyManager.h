#pragma once

#include "PlayerState.h"

#include <cstddef>
#include <cstdint>

namespace SkyrimMP
{
    struct RemotePlayerProxyUpdate
    {
        std::uint64_t networkEntityId{};
        std::uint64_t revision{};
        Vec3 position{};
        Vec3 rotation{};
        std::uint32_t cellFormId{};
        std::uint32_t worldspaceFormId{};
        float health{};
        float magicka{};
        float stamina{};
        bool dead{};
        bool inCombat{};
        bool hasActorState{};
        bool hasStatusState{};
        std::uint16_t actionFlags{};
        std::vector<std::uint32_t> equippedFormIds;
    };

    class RemotePlayerProxyManager
    {
    public:
        static void EnqueueUpsert(const RemotePlayerProxyUpdate& a_update);
        static void EnqueueAppearance(std::uint64_t a_networkEntityId, const PlayerAppearance& a_appearance);
        static void EnqueueDespawn(std::uint64_t a_networkEntityId);
        static std::size_t ApplyPending(std::size_t a_budget = 8);
        static void Reset();
    };
}
