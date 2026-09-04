#include "pch.h"

#include "MultiplayerLaunchConfig.h"

#include <charconv>
#include <fstream>

namespace SkyrimMP
{
    namespace
    {
        std::string Trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        MultiplayerLaunchConfig ReadConfig()
        {
            MultiplayerLaunchConfig config;
            std::ifstream input("Data/SKSE/Plugins/SkyrimMPClient.ini");
            std::string line;
            while (std::getline(input, line)) {
                const auto equals = line.find('=');
                if (equals == std::string::npos) continue;
                const auto key = Trim(line.substr(0, equals));
                const auto value = Trim(line.substr(equals + 1));
                if (key == "Mode") {
                    config.createCharacter = value == "Create" || value == "create";
                } else if (key == "CharacterId") {
                    std::uint64_t parsed{};
                    const auto* begin = value.data();
                    const auto* end = begin + value.size();
                    const auto result = std::from_chars(begin, end, parsed, 16);
                    if (result.ec == std::errc{} && result.ptr == end) config.characterId = parsed;
                } else if (key == "SaveName") {
                    config.saveName = value;
                }
            }
            if (config.saveName.empty() && config.characterId != 0) {
                char name[40]{};
                std::snprintf(name, sizeof(name), "SkyrimMP_%016llX", static_cast<unsigned long long>(config.characterId));
                config.saveName = name;
            }
            return config;
        }
    }

    const MultiplayerLaunchConfig& GetMultiplayerLaunchConfig()
    {
        static const auto config = ReadConfig();
        return config;
    }
}
