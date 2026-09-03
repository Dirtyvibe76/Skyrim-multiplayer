#include "FormIdResolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kEslFlag = 0x00000200u;

        std::string Lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        bool IsLightPlugin(const PluginStackEntry& a_entry)
        {
            const auto extension = Lower(a_entry.path.extension().string());
            return extension == ".esl" || (a_entry.header.recordFlags & kEslFlag) != 0;
        }
    }

    const char* FormNamespaceKindName(FormNamespaceKind a_kind)
    {
        switch (a_kind) {
        case FormNamespaceKind::Full:
            return "full";
        case FormNamespaceKind::Light:
            return "light";
        }
        return "unknown";
    }

    std::vector<PluginNamespace> BuildFormNamespaces(
        const std::vector<PluginStackEntry>& a_stack)
    {
        std::vector<PluginNamespace> result;
        result.reserve(a_stack.size());

        std::uint32_t fullIndex = 0;
        std::uint32_t lightIndex = 0;

        for (const auto& entry : a_stack) {
            PluginNamespace ns;
            ns.stackIndex = entry.stackIndex;
            ns.pluginName = entry.header.filename;

            if (IsLightPlugin(entry)) {
                ns.kind = FormNamespaceKind::Light;
                ns.namespaceIndex = lightIndex++;
                ns.localIdBits = 12;
            } else {
                ns.kind = FormNamespaceKind::Full;
                ns.namespaceIndex = fullIndex++;
                ns.localIdBits = 24;
            }

            result.push_back(std::move(ns));
        }

        return result;
    }

    CanonicalFormReference ResolvePluginFormReference(
        std::uint32_t a_rawFormId,
        const PluginStackEntry& a_sourcePlugin,
        const std::vector<PluginStackEntry>& a_stack,
        const std::vector<PluginNamespace>& a_namespaces)
    {
        CanonicalFormReference result;
        result.rawFormId = a_rawFormId;
        if (a_rawFormId == 0) {
            result.isNull = true;
            result.resolved = true;
            return result;
        }
        if (a_stack.size() != a_namespaces.size()) {
            return result;
        }

        const std::uint32_t fileIndex = a_rawFormId >> 24;
        std::string ownerName;
        if (fileIndex < a_sourcePlugin.header.masters.size()) {
            ownerName = a_sourcePlugin.header.masters[fileIndex];
        } else if (fileIndex == a_sourcePlugin.header.masters.size()) {
            ownerName = a_sourcePlugin.header.filename;
        } else {
            return result;
        }

        const auto ownerKey = Lower(ownerName);
        for (std::size_t i = 0; i < a_stack.size(); ++i) {
            if (Lower(a_stack[i].header.filename) != ownerKey) {
                continue;
            }

            const auto& ns = a_namespaces[i];
            result.kind = ns.kind;
            result.namespaceIndex = ns.namespaceIndex;
            result.pluginName = a_stack[i].header.filename;
            const std::uint32_t mask = ns.localIdBits == 12 ? 0x00000FFFu : 0x00FFFFFFu;
            result.localId = a_rawFormId & mask;
            result.resolved = true;
            return result;
        }

        return result;
    }
}
