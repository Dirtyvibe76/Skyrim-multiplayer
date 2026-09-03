#pragma once

#include "PlayerState.h"

#include <cstdint>

namespace SkyrimMP
{
    struct RemoteTransform
    {
        std::uint32_t runtimeFormId{};
        std::uint32_t sequence{};
        Vec3 position{};
        Vec3 rotation{};
    };
}
