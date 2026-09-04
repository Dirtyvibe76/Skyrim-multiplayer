#pragma once

#include <cstdint>

namespace SkyrimMP::BuildInfo
{
    inline constexpr auto kVersion = "0.1.0-alpha.2";
    inline constexpr auto kChannel = "re-0.1-runtime-probe";
    inline constexpr std::uint16_t kWireProtocol = 2;
    inline constexpr std::uint16_t kReplicationProtocol = 8;
}
