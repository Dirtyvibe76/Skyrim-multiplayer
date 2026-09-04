#pragma once

#include "CanonicalRecordDatabase.h"
#include "PluginStack.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    enum class QuestCompatibility : std::uint8_t { RecordDriven, AdapterRequired, Unsupported };

    struct ImportedQuestStage
    {
        std::uint16_t index{};
        std::uint32_t logEntries{};
        std::uint32_t conditions{};
        bool hasScriptFragment{};
    };

    struct ImportedQuestObjective
    {
        std::uint32_t index{};
        std::uint32_t targetCount{};
        std::uint32_t conditions{};
    };

    enum class ImportedQuestAliasKind : std::uint8_t { Reference, Location, Collection };

    struct ImportedQuestAlias
    {
        std::uint32_t id{};
        ImportedQuestAliasKind kind{ ImportedQuestAliasKind::Reference };
        std::uint32_t conditions{};
        bool hasScript{};
    };

    struct ImportedQuestDefinition
    {
        CanonicalRecordKey key;
        std::string plugin;
        std::string editorId;
        std::vector<ImportedQuestStage> stages;
        std::vector<ImportedQuestObjective> objectives;
        std::vector<ImportedQuestAlias> aliases;
        std::uint32_t questConditions{};
        std::uint32_t scriptBytes{};
        std::uint32_t unknownSubrecords{};
        QuestCompatibility compatibility{ QuestCompatibility::RecordDriven };
        std::vector<std::string> compatibilityReasons;
    };

    struct ImportedQuestDatabase
    {
        std::map<std::string, ImportedQuestDefinition> byStableKey;
        std::uint64_t recordDriven{};
        std::uint64_t adapterRequired{};
        std::uint64_t unsupported{};
        std::uint64_t stages{};
        std::uint64_t objectives{};
        std::uint64_t aliases{};
        std::uint64_t conditions{};
    };

    ImportedQuestDatabase BuildImportedQuestDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack);

    const char* QuestCompatibilityName(QuestCompatibility a_compatibility);
}
