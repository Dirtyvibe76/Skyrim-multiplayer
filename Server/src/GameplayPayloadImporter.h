#pragma once

#include "CanonicalRecordDatabase.h"
#include "ImportedQuestDatabase.h"
#include "ServerQuestProgram.h"
#include "PluginStack.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct RuntimeEntityRegistry;

    struct GameplayPayloadSample
    {
        std::string type;
        std::string plugin;
        std::string editorId;
        CanonicalRecordKey key;
        std::uint32_t subrecordCount{};
        bool compressed{};
    };

    struct GameplayPayloadSummary
    {
        std::uint64_t candidateRecords{};
        std::uint64_t parsedRecords{};
        std::uint64_t compressedRecords{};
        std::uint64_t compressedBytes{};
        std::uint64_t decompressedBytes{};
        std::uint64_t subrecords{};
        std::uint64_t editorIds{};
        std::map<std::string, std::uint64_t> typeCounts;
        std::vector<GameplayPayloadSample> samples;
        std::shared_ptr<const ImportedQuestDatabase> questDefinitions;
        std::shared_ptr<const ServerQuestProgramDatabase> questPrograms;
    };

    GameplayPayloadSummary ImportGameplayPayloads(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack,
        std::size_t a_sampleLimit = 12,
        RuntimeEntityRegistry* a_runtimeRegistry = nullptr);
}
