#pragma once

#include "CanonicalRecordDatabase.h"
#include "PluginStack.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct GameplayPayloadSample
    {
        std::string type;
        std::string plugin;
        std::string editorId;
        CanonicalRecordKey key;
        std::uint32_t subrecordCount{};
    };

    struct GameplayPayloadSummary
    {
        std::uint64_t candidateRecords{};
        std::uint64_t parsedRecords{};
        std::uint64_t deferredCompressed{};
        std::uint64_t subrecords{};
        std::uint64_t editorIds{};
        std::map<std::string, std::uint64_t> typeCounts;
        std::vector<GameplayPayloadSample> samples;
    };

    GameplayPayloadSummary ImportGameplayPayloads(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack,
        std::size_t a_sampleLimit = 12);
}
