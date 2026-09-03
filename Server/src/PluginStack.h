#pragma once

#include "BethesdaPlugin.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    enum class PluginSource
    {
        BaseGame,
        HostedMod
    };

    struct PluginStackEntry
    {
        std::uint32_t stackIndex{};
        PluginSource source{ PluginSource::HostedMod };
        std::filesystem::path path;
        BethesdaPluginHeader header;
    };

    struct PluginLoadOrderInfo
    {
        bool explicitPlugins{};
        bool explicitLoadOrder{};
        std::vector<std::string> enabledHostedPlugins;
        std::string revisionHash;
    };

    std::vector<PluginStackEntry> ResolvePluginStack(
        const std::vector<std::filesystem::path>& a_hostedPlugins,
        const std::filesystem::path& a_gameDataPath);

    const PluginLoadOrderInfo& GetLastPluginLoadOrderInfo();
    const char* PluginSourceName(PluginSource a_source);
}
