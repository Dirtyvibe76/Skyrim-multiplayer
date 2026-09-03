#pragma once

#include "PluginStack.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    enum class FormNamespaceKind
    {
        Full,
        Light
    };

    struct PluginNamespace
    {
        std::uint32_t stackIndex{};
        FormNamespaceKind kind{ FormNamespaceKind::Full };
        std::uint32_t namespaceIndex{};
        std::uint32_t localIdBits{};
        std::string pluginName;
    };

    std::vector<PluginNamespace> BuildFormNamespaces(
        const std::vector<PluginStackEntry>& a_stack);

    const char* FormNamespaceKindName(FormNamespaceKind a_kind);
}
