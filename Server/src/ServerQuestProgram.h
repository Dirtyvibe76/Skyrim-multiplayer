#pragma once

#include "ImportedQuestDatabase.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct ServerQuestStageProgram
    {
        std::uint16_t index{};
        std::optional<std::uint16_t> nextStage;
        std::uint32_t logEntries{};
        std::uint32_t conditionCount{};
    };

    struct ServerQuestProgram
    {
        CanonicalRecordKey quest;
        std::string stableKey;
        std::string editorId;
        std::vector<ServerQuestStageProgram> stages;
        std::vector<std::uint32_t> objectives;
    };

    struct ServerQuestProgramDatabase
    {
        std::map<std::string, ServerQuestProgram> programs;
        std::uint64_t skippedAdapterRequired{};
        std::uint64_t skippedUnsupported{};
        std::uint64_t skippedNoStages{};
        std::uint64_t duplicateStageEntriesMerged{};
    };

    ServerQuestProgramDatabase CompileServerQuestPrograms(const ImportedQuestDatabase& a_imported);
}
