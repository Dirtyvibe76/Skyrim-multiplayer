#pragma once

#include <cstdint>

namespace SkyrimMP
{
    using WorldEntityId = std::uint64_t;

    inline constexpr WorldEntityId kDynamicWorldEntityBit = 1ull << 63;

    constexpr WorldEntityId MakeStaticWorldEntityId(
        bool a_lightNamespace,
        std::uint32_t a_namespaceIndex,
        std::uint32_t a_localId) noexcept
    {
        const std::uint64_t light = a_lightNamespace ? 1ull : 0ull;
        return 1ull + (light << 62) + (static_cast<std::uint64_t>(a_namespaceIndex) << 32) + a_localId;
    }

    constexpr bool IsDynamicWorldEntity(WorldEntityId a_id) noexcept
    {
        return a_id != 0 && (a_id & kDynamicWorldEntityBit) != 0;
    }
}
