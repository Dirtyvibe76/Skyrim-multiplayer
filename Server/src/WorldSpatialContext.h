#pragma once

#include "CanonicalRecordDatabase.h"
#include "FormIdResolver.h"
#include "PluginStack.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimMP::Server
{
    struct WorldSpatialContext
    {
        CanonicalRecordKey referenceKey;
        CanonicalFormReference cell;
        CanonicalFormReference worldspace;
        bool hasCell{};
        bool hasWorldspace{};
    };

    struct WorldSpatialContextDatabase
    {
        std::unordered_map<CanonicalRecordKey, WorldSpatialContext, CanonicalRecordKeyHash> contexts;
        std::uint64_t referenceRecords{};
        std::uint64_t withCell{};
        std::uint64_t withoutCell{};
        std::uint64_t interiorReferences{};
        std::uint64_t exteriorReferences{};
        std::uint64_t withWorldspace{};
        std::uint64_t cellTargetsValidated{};
        std::uint64_t worldspaceTargetsValidated{};
        std::uint64_t missingTargets{};
        std::uint64_t cellTypeMismatches{};
        std::uint64_t worldspaceTypeMismatches{};
        std::unordered_set<CanonicalRecordKey, CanonicalRecordKeyHash> uniqueCells;
        std::unordered_set<CanonicalRecordKey, CanonicalRecordKeyHash> uniqueWorldspaces;
    };

    WorldSpatialContextDatabase BuildWorldSpatialContextDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack);
}
