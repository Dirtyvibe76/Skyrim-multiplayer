#pragma once

#include <cstdint>

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
        std::uint32_t formId{};
        std::uint32_t cellFormId{};
        std::uint32_t worldspaceFormId{};

        Vec3 position{};
        Vec3 rotation{};
    };
}
