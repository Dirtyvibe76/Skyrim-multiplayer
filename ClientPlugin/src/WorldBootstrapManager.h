#pragma once

#include "PlayerState.h"

#include <cstddef>
#include <cstdint>

namespace SkyrimMP
{
    struct ServerWorldBootstrap
    {
        std::uint64_t playerEntityId{};
        std::uint32_t anchorRuntimeFormId{};
        std::uint32_t cellFormId{};
        std::uint32_t worldspaceFormId{};
        Vec3 position{};
        Vec3 rotation{};
    };

    class WorldBootstrapManager
    {
    public:
        static void Enqueue(const ServerWorldBootstrap& a_bootstrap);
        static std::size_t ApplyPending();
        static void Reset();
        static bool HasApplied();
    };
}
