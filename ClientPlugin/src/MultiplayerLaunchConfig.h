#pragma once

#include <cstdint>
#include <string>

namespace SkyrimMP
{
    struct MultiplayerLaunchConfig
    {
        bool createCharacter{};
        std::uint64_t characterId{};
        std::string saveName;
    };

    const MultiplayerLaunchConfig& GetMultiplayerLaunchConfig();
}
