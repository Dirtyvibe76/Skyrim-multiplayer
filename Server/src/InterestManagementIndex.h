#pragma once

#include "CanonicalRecordDatabase.h"
#include "WorldReferenceDatabase.h"
#include "WorldSpatialContext.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    constexpr float kExteriorCellSize = 4096.0f;

    struct ExteriorInterestBucketKey
    {
        CanonicalRecordKey worldspace;
        std::int32_t gridX{};
        std::int32_t gridY{};

        bool operator==(const ExteriorInterestBucketKey&) const = default;
    };

    struct ExteriorInterestBucketKeyHash
    {
        std::size_t operator()(const ExteriorInterestBucketKey& a_key) const noexcept;
    };

    struct InterestManagementIndex
    {
        std::unordered_map<CanonicalRecordKey, std::vector<CanonicalRecordKey>, CanonicalRecordKeyHash> interiorCells;
        std::unordered_map<ExteriorInterestBucketKey, std::vector<CanonicalRecordKey>, ExteriorInterestBucketKeyHash> exteriorBuckets;

        std::uint64_t sourceRecords{};
        std::uint64_t activeRecords{};
        std::uint64_t deletedSkipped{};
        std::uint64_t interiorIndexed{};
        std::uint64_t exteriorIndexed{};
        std::uint64_t missingSpatialContext{};
        std::uint64_t missingTransform{};
        std::uint64_t duplicateAssignments{};
        std::uint64_t largestInteriorCell{};
        std::uint64_t largestExteriorBucket{};
    };

    InterestManagementIndex BuildInterestManagementIndex(
        const WorldReferenceDatabase& a_world,
        const WorldSpatialContextDatabase& a_spatial);

    const std::vector<CanonicalRecordKey>* FindInteriorInterestSet(
        const InterestManagementIndex& a_index,
        const CanonicalRecordKey& a_cell);

    std::vector<CanonicalRecordKey> CollectExteriorInterestSet(
        const InterestManagementIndex& a_index,
        const CanonicalRecordKey& a_worldspace,
        std::int32_t a_gridX,
        std::int32_t a_gridY,
        std::int32_t a_radiusCells = 1);
}
