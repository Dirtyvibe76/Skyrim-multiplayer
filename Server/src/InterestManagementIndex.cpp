#include "InterestManagementIndex.h"

#include "FormIdResolver.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace SkyrimMP::Server
{
    namespace
    {
        CanonicalRecordKey ToKey(const CanonicalFormReference& a_reference)
        {
            return CanonicalRecordKey{ a_reference.kind, a_reference.namespaceIndex, a_reference.localId };
        }

        std::int32_t ToExteriorGrid(float a_coordinate)
        {
            return static_cast<std::int32_t>(std::floor(static_cast<double>(a_coordinate) / static_cast<double>(kExteriorCellSize)));
        }

        template <class Map>
        std::uint64_t LargestBucket(const Map& a_map)
        {
            std::uint64_t largest = 0;
            for (const auto& [key, records] : a_map) {
                (void)key;
                largest = std::max<std::uint64_t>(largest, records.size());
            }
            return largest;
        }
    }

    std::size_t ExteriorInterestBucketKeyHash::operator()(const ExteriorInterestBucketKey& a_key) const noexcept
    {
        std::size_t seed = CanonicalRecordKeyHash{}(a_key.worldspace);
        seed ^= std::hash<std::int32_t>{}(a_key.gridX) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        seed ^= std::hash<std::int32_t>{}(a_key.gridY) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }

    const std::vector<CanonicalRecordKey>* FindInteriorInterestSet(
        const InterestManagementIndex& a_index,
        const CanonicalRecordKey& a_cell)
    {
        const auto it = a_index.interiorCells.find(a_cell);
        return it == a_index.interiorCells.end() ? nullptr : &it->second;
    }

    std::vector<CanonicalRecordKey> CollectExteriorInterestSet(
        const InterestManagementIndex& a_index,
        const CanonicalRecordKey& a_worldspace,
        std::int32_t a_gridX,
        std::int32_t a_gridY,
        std::int32_t a_radiusCells)
    {
        if (a_radiusCells < 0) throw std::runtime_error("exterior interest radius cannot be negative");

        std::vector<CanonicalRecordKey> result;
        for (std::int32_t y = a_gridY - a_radiusCells; y <= a_gridY + a_radiusCells; ++y) {
            for (std::int32_t x = a_gridX - a_radiusCells; x <= a_gridX + a_radiusCells; ++x) {
                const ExteriorInterestBucketKey key{ a_worldspace, x, y };
                const auto it = a_index.exteriorBuckets.find(key);
                if (it != a_index.exteriorBuckets.end()) {
                    result.insert(result.end(), it->second.begin(), it->second.end());
                }
            }
        }
        return result;
    }

    InterestManagementIndex BuildInterestManagementIndex(
        const WorldReferenceDatabase& a_world,
        const WorldSpatialContextDatabase& a_spatial)
    {
        InterestManagementIndex index;
        index.sourceRecords = a_world.parsedRecords;

        std::unordered_set<CanonicalRecordKey, CanonicalRecordKeyHash> assigned;
        assigned.reserve(static_cast<std::size_t>(a_world.parsedRecords));

        auto indexRecord = [&](const WorldReferenceRecord& record) {
            if (record.deleted) {
                ++index.deletedSkipped;
                return;
            }
            ++index.activeRecords;

            if (!record.hasTransform) {
                ++index.missingTransform;
                throw std::runtime_error("active world record has no transform while building interest index");
            }

            const auto contextIt = a_spatial.contexts.find(record.key);
            if (contextIt == a_spatial.contexts.end()) {
                ++index.missingSpatialContext;
                throw std::runtime_error("active world record has no spatial context while building interest index");
            }
            const auto& context = contextIt->second;

            if (!assigned.insert(record.key).second) {
                ++index.duplicateAssignments;
                throw std::runtime_error("world record assigned to multiple interest buckets");
            }

            if (context.hasWorldspace) {
                if (!context.hasCell) throw std::runtime_error("exterior world record is missing CELL context");
                const auto gridX = ToExteriorGrid(record.transform.x);
                const auto gridY = ToExteriorGrid(record.transform.y);
                const ExteriorInterestBucketKey bucket{ ToKey(context.worldspace), gridX, gridY };
                index.exteriorBuckets[bucket].push_back(record.key);
                ++index.exteriorIndexed;
            } else {
                if (!context.hasCell) throw std::runtime_error("interior world record is missing CELL context");
                index.interiorCells[ToKey(context.cell)].push_back(record.key);
                ++index.interiorIndexed;
            }
        };

        for (const auto& record : a_world.references) indexRecord(record);
        for (const auto& record : a_world.actors) indexRecord(record);

        if (index.activeRecords + index.deletedSkipped != index.sourceRecords) {
            throw std::runtime_error("interest index source partition invariant failed");
        }
        if (index.interiorIndexed + index.exteriorIndexed != index.activeRecords) {
            throw std::runtime_error("interest index active partition invariant failed");
        }
        if (assigned.size() != index.activeRecords) {
            throw std::runtime_error("interest index assignment-count invariant failed");
        }
        if (index.missingSpatialContext != 0 || index.missingTransform != 0 || index.duplicateAssignments != 0) {
            throw std::runtime_error("interest index integrity invariant failed");
        }

        index.largestInteriorCell = LargestBucket(index.interiorCells);
        index.largestExteriorBucket = LargestBucket(index.exteriorBuckets);

        std::cout << "[INTEREST] source=" << index.sourceRecords
                  << " active=" << index.activeRecords
                  << " deletedSkipped=" << index.deletedSkipped
                  << " interiorIndexed=" << index.interiorIndexed
                  << " exteriorIndexed=" << index.exteriorIndexed
                  << " interiorCells=" << index.interiorCells.size()
                  << " exteriorBuckets=" << index.exteriorBuckets.size()
                  << " largestInteriorCell=" << index.largestInteriorCell
                  << " largestExteriorBucket=" << index.largestExteriorBucket
                  << " missingSpatial=" << index.missingSpatialContext
                  << " missingTransform=" << index.missingTransform
                  << " duplicateAssignments=" << index.duplicateAssignments << '\n';

        if (!index.interiorCells.empty()) {
            const auto& [cell, records] = *index.interiorCells.begin();
            const auto* found = FindInteriorInterestSet(index, cell);
            if (found == nullptr || found->size() != records.size()) {
                throw std::runtime_error("interior interest lookup self-test failed");
            }
            std::cout << "[INTEREST-SAMPLE] interior cell=" << FormNamespaceKindName(cell.kind)
                      << ':' << cell.namespaceIndex << ":0x" << std::hex << std::uppercase << cell.localId
                      << std::dec << std::nouppercase << " records=" << records.size() << '\n';
        }

        if (!index.exteriorBuckets.empty()) {
            const auto& [bucket, records] = *index.exteriorBuckets.begin();
            const auto nearby = CollectExteriorInterestSet(index, bucket.worldspace, bucket.gridX, bucket.gridY, 1);
            if (nearby.size() < records.size()) throw std::runtime_error("exterior interest lookup self-test failed");
            std::cout << "[INTEREST-SAMPLE] exterior world=" << FormNamespaceKindName(bucket.worldspace.kind)
                      << ':' << bucket.worldspace.namespaceIndex << ":0x" << std::hex << std::uppercase << bucket.worldspace.localId
                      << std::dec << std::nouppercase
                      << " grid=(" << bucket.gridX << ',' << bucket.gridY << ')'
                      << " bucketRecords=" << records.size()
                      << " radius1Records=" << nearby.size() << '\n';
        }

        return index;
    }
}
