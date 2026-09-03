#pragma once

#include "FormIdResolver.h"
#include "PluginStack.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct CanonicalFormId
    {
        FormNamespaceKind kind{ FormNamespaceKind::Full };
        std::uint32_t namespaceIndex{};
        std::uint32_t localId{};
        std::string pluginName;
        bool resolved{};
    };

    struct BethesdaRecordSummary
    {
        std::string pluginName;
        std::uint32_t recordCount{};
        std::uint32_t canonicalResolved{};
        std::uint32_t canonicalUnresolved{};
        std::map<std::string, std::uint32_t> typeCounts;
        std::vector<std::pair<std::uint32_t, CanonicalFormId>> samples;
    };

    BethesdaRecordSummary ScanBethesdaRecords(
        const PluginStackEntry& a_plugin,
        const std::vector<PluginStackEntry>& a_stack,
        const std::vector<PluginNamespace>& a_namespaces,
        std::size_t a_sampleLimit = 8);
}
