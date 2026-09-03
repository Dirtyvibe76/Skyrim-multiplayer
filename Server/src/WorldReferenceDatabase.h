#pragma once

#include "CanonicalRecordDatabase.h"
#include "FormIdResolver.h"
#include "PluginStack.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct WorldTransform
    {
        float x{};
        float y{};
        float z{};
        float pitch{};
        float yaw{};
        float roll{};
    };

    struct WorldReferenceRecord
    {
        CanonicalRecordKey key;
        std::string plugin;
        std::string editorId;
        CanonicalFormReference baseObject;
        WorldTransform transform;
        bool hasBaseObject{};
        bool hasTransform{};
        bool deleted{};
    };

    struct WorldReferenceDatabase
    {
        std::vector<WorldReferenceRecord> references;
        std::vector<WorldReferenceRecord> actors;
        std::uint64_t parsedRecords{};
        std::uint64_t deletedRecords{};
        std::uint64_t baseReferences{};
        std::uint64_t baseTargetsValidated{};
        std::uint64_t missingBaseTargets{};
        std::uint64_t actorTypeChecks{};
        std::uint64_t actorTypeMismatches{};
        std::uint64_t transformRecords{};
        std::uint64_t missingBaseObjects{};
        std::uint64_t missingTransforms{};
        std::map<std::string, std::uint64_t> referenceBaseTypeCounts;
    };

    WorldReferenceDatabase BuildWorldReferenceDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack);
}
