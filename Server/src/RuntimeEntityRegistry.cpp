#include "RuntimeEntityRegistry.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace SkyrimMP::Server
{
    namespace
    {
        std::int32_t ToGrid(float coordinate)
        {
            return static_cast<std::int32_t>(std::floor(static_cast<double>(coordinate) / static_cast<double>(kExteriorCellSize)));
        }

        ExteriorInterestBucketKey ExteriorKey(const RuntimeEntityState& entity)
        {
            return ExteriorInterestBucketKey{ entity.location.worldspace, ToGrid(entity.transform.x), ToGrid(entity.transform.y) };
        }

        void AddToBucket(RuntimeEntityRegistry& registry, const RuntimeEntityState& entity)
        {
            if (!entity.location.hasCell) throw std::runtime_error("runtime entity missing CELL context");
            if (entity.location.exterior) {
                if (!entity.location.hasWorldspace) throw std::runtime_error("exterior runtime entity missing WRLD context");
                registry.exteriorBuckets[ExteriorKey(entity)].push_back(entity.id);
            } else {
                registry.interiorCells[entity.location.cell].push_back(entity.id);
            }
        }

        void RemoveFromBucket(RuntimeEntityRegistry& registry, const RuntimeEntityState& entity)
        {
            auto eraseId = [&](auto& map, const auto& key) {
                const auto it = map.find(key);
                if (it == map.end()) throw std::runtime_error("runtime entity bucket missing during removal");
                auto& ids = it->second;
                const auto pos = std::find(ids.begin(), ids.end(), entity.id);
                if (pos == ids.end()) throw std::runtime_error("runtime entity id missing from bucket during removal");
                *pos = ids.back();
                ids.pop_back();
                if (ids.empty()) map.erase(it);
            };

            if (entity.location.exterior) eraseId(registry.exteriorBuckets, ExteriorKey(entity));
            else eraseId(registry.interiorCells, entity.location.cell);
        }

        bool SameBucket(const RuntimeEntityState& entity, const WorldTransform& transform, const RuntimeEntityLocation& location)
        {
            if (entity.location.exterior != location.exterior || entity.location.hasCell != location.hasCell ||
                entity.location.hasWorldspace != location.hasWorldspace || entity.location.cell != location.cell ||
                entity.location.worldspace != location.worldspace) return false;
            if (!location.exterior) return true;
            return ToGrid(entity.transform.x) == ToGrid(transform.x) && ToGrid(entity.transform.y) == ToGrid(transform.y);
        }

        NetworkEntityId StaticNetworkId(const CanonicalRecordKey& key)
        {
            const std::uint64_t light = key.kind == FormNamespaceKind::Light ? 1ull : 0ull;
            return 1ull + (light << 62) + (static_cast<std::uint64_t>(key.namespaceIndex) << 32) + key.localId;
        }
    }

    RuntimeEntityRegistry BuildRuntimeEntityRegistry(
        const WorldReferenceDatabase& a_world,
        const WorldSpatialContextDatabase& a_spatial)
    {
        RuntimeEntityRegistry registry;
        registry.entities.reserve(static_cast<std::size_t>(a_world.parsedRecords));
        registry.sourceToNetwork.reserve(static_cast<std::size_t>(a_world.parsedRecords));

        auto addStatic = [&](const WorldReferenceRecord& record, RuntimeEntityKind kind) {
            if (record.deleted) return;
            const auto spatial = a_spatial.contexts.find(record.key);
            if (spatial == a_spatial.contexts.end()) throw std::runtime_error("runtime registry missing spatial context");
            if (!record.hasTransform) throw std::runtime_error("runtime registry active source has no transform");

            RuntimeEntityState entity;
            entity.id = StaticNetworkId(record.key);
            entity.kind = kind;
            entity.sourceRecord = record.key;
            entity.hasSourceRecord = true;
            entity.transform = record.transform;
            entity.location.hasCell = spatial->second.hasCell;
            entity.location.hasWorldspace = spatial->second.hasWorldspace;
            entity.location.exterior = spatial->second.hasWorldspace;
            if (spatial->second.hasCell) entity.location.cell = CanonicalRecordKey{ spatial->second.cell.kind, spatial->second.cell.namespaceIndex, spatial->second.cell.localId };
            if (spatial->second.hasWorldspace) entity.location.worldspace = CanonicalRecordKey{ spatial->second.worldspace.kind, spatial->second.worldspace.namespaceIndex, spatial->second.worldspace.localId };

            if (!registry.entities.emplace(entity.id, entity).second) throw std::runtime_error("duplicate NetworkEntityId while seeding runtime registry");
            if (!registry.sourceToNetwork.emplace(record.key, entity.id).second) throw std::runtime_error("duplicate source record while seeding runtime registry");
            AddToBucket(registry, entity);
            ++registry.staticEntities;
            if (kind == RuntimeEntityKind::Actor) ++registry.actors;
            else ++registry.references;
        };

        for (const auto& record : a_world.references) addStatic(record, RuntimeEntityKind::StaticReference);
        for (const auto& record : a_world.actors) addStatic(record, RuntimeEntityKind::Actor);

        if (registry.staticEntities != a_world.parsedRecords - a_world.deletedRecords) {
            throw std::runtime_error("runtime registry static entity count invariant failed");
        }
        if (registry.entities.size() != registry.staticEntities || registry.sourceToNetwork.size() != registry.staticEntities) {
            throw std::runtime_error("runtime registry identity invariant failed");
        }

        std::cout << "[ENTITY] static=" << registry.staticEntities
                  << " references=" << registry.references
                  << " actors=" << registry.actors
                  << " interiorBuckets=" << registry.interiorCells.size()
                  << " exteriorBuckets=" << registry.exteriorBuckets.size()
                  << " dynamicBase=0x" << std::hex << std::uppercase << registry.nextDynamicId
                  << std::dec << std::nouppercase << '\n';

        RuntimeEntityLocation firstInterior;
        RuntimeEntityLocation firstExterior;
        bool haveInterior = false;
        bool haveExterior = false;
        WorldTransform interiorTransform{};
        WorldTransform exteriorTransform{};
        for (const auto& [id, entity] : registry.entities) {
            (void)id;
            if (!entity.location.exterior && !haveInterior) {
                firstInterior = entity.location;
                interiorTransform = entity.transform;
                haveInterior = true;
            }
            if (entity.location.exterior && !haveExterior) {
                firstExterior = entity.location;
                exteriorTransform = entity.transform;
                haveExterior = true;
            }
            if (haveInterior && haveExterior) break;
        }

        if (haveInterior && haveExterior) {
            const auto testId = SpawnRuntimeEntity(registry, RuntimeEntityKind::Player, interiorTransform, firstInterior);
            if (!UpdateRuntimeEntity(registry, testId, exteriorTransform, firstExterior)) throw std::runtime_error("runtime entity transition self-test update failed");
            if (!DespawnRuntimeEntity(registry, testId)) throw std::runtime_error("runtime entity transition self-test despawn failed");
            if (registry.entities.size() != registry.staticEntities) throw std::runtime_error("runtime entity self-test leaked dynamic entity");
            std::cout << "[ENTITY-SELFTEST] spawn=true interiorToExterior=true rebucket=true despawn=true\n";
        }

        return registry;
    }

    NetworkEntityId SpawnRuntimeEntity(RuntimeEntityRegistry& registry, RuntimeEntityKind kind, const WorldTransform& transform, const RuntimeEntityLocation& location)
    {
        if (!location.hasCell) throw std::runtime_error("cannot spawn runtime entity without CELL context");
        if (location.exterior && !location.hasWorldspace) throw std::runtime_error("cannot spawn exterior runtime entity without WRLD context");
        const auto id = registry.nextDynamicId++;
        RuntimeEntityState entity;
        entity.id = id;
        entity.kind = kind;
        entity.transform = transform;
        entity.location = location;
        entity.revision = 1;
        if (!registry.entities.emplace(id, entity).second) throw std::runtime_error("dynamic NetworkEntityId collision");
        AddToBucket(registry, registry.entities.at(id));
        ++registry.spawned;
        return id;
    }

    bool DespawnRuntimeEntity(RuntimeEntityRegistry& registry, NetworkEntityId id)
    {
        const auto it = registry.entities.find(id);
        if (it == registry.entities.end()) return false;
        RemoveFromBucket(registry, it->second);
        if (it->second.hasSourceRecord) registry.sourceToNetwork.erase(it->second.sourceRecord);
        registry.entities.erase(it);
        ++registry.despawned;
        return true;
    }

    bool UpdateRuntimeEntity(RuntimeEntityRegistry& registry, NetworkEntityId id, const WorldTransform& transform, const RuntimeEntityLocation& location)
    {
        const auto it = registry.entities.find(id);
        if (it == registry.entities.end()) return false;
        auto& entity = it->second;
        const bool rebucket = !SameBucket(entity, transform, location);
        if (rebucket) RemoveFromBucket(registry, entity);
        entity.transform = transform;
        entity.location = location;
        ++entity.revision;
        ++registry.updates;
        if (rebucket) {
            AddToBucket(registry, entity);
            ++registry.rebuckets;
        }
        return true;
    }
}
