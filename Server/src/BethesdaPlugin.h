#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct BethesdaPluginHeader
    {
        std::string filename;
        std::uint32_t recordFlags{};
        float headerVersion{};
        std::uint32_t recordCount{};
        std::uint32_t nextObjectId{};
        std::vector<std::string> masters;
    };

    BethesdaPluginHeader ParseBethesdaPluginHeader(const std::filesystem::path& a_path);
}
