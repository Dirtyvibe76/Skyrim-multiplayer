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
    };

    class RemotePlayerProxyManager
    {
    public:
        static void EnqueueUpsert(const RemotePlayerProxyUpdate& a_update);
        static void EnqueueDespawn(std::uint64_t a_networkEntityId);
        static std::size_t ApplyPending(std::size_t a_budget = 8);
        static void Reset();
    };
}
