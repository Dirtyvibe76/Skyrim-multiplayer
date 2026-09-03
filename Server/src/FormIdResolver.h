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

    struct CanonicalFormReference
    {
        std::uint32_t rawFormId{};
        FormNamespaceKind kind{ FormNamespaceKind::Full };
        std::uint32_t namespaceIndex{};
        std::uint32_t localId{};
        std::string pluginName;
        bool isNull{};
        bool resolved{};
    };

    std::vector<PluginNamespace> BuildFormNamespaces(
        const std::vector<PluginStackEntry>& a_stack);

    CanonicalFormReference ResolvePluginFormReference(
        std::uint32_t a_rawFormId,
        const PluginStackEntry& a_sourcePlugin,
        const std::vector<PluginStackEntry>& a_stack,
        const std::vector<PluginNamespace>& a_namespaces);

    const char* FormNamespaceKindName(FormNamespaceKind a_kind);
}
