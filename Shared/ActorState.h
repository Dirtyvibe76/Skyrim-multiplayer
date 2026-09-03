#pragma once

#include "PlayerState.h"

#include <cstdint>

namespace SkyrimMP
{
    struct ActorState
    {
        std::uint32_t runtimeFormId{};
        std::uint32_t baseFormId{};
        std::uint32_t cellFormId{};
        std::uint32_t worldspaceFormId{};
        Vec3 position{};
        Vec3 rotation{};
    };
}
