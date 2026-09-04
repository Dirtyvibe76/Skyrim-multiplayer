#pragma once

#include "CanonicalRecordDatabase.h"
#include "InterestManagementIndex.h"
#include "WorldReferenceDatabase.h"
#include "WorldSpatialContext.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    struct ImportedQuestDatabase;
    using NetworkEntityId = std::uint64_t;

    enum class RuntimeEntityKind : std::uint8_t
    {
        StaticReference,
        Actor,
        Player
    };

    struct RuntimeEntityLocation
    {
        CanonicalRecordKey cell;
        CanonicalRecordKey worldspace;
        bool exterior{};
        bool hasCell{};
        bool hasWorldspace{};
    };

    struct RuntimeEntityState
    {
        NetworkEntityId id{};
        RuntimeEntityKind kind{ RuntimeEntityKind::StaticReference };
        CanonicalRecordKey sourceRecord;
        bool hasSourceRecord{};
        WorldTransform transform;
        RuntimeEntityLocation location;
        float health{};
        float magicka{};
        float stamina{};
        bool dead{};
        bool inCombat{};
        bool hasActorState{};
        bool hasStatusState{};
        std::vector<std::uint32_t> equippedFormIds;
        std::uint64_t revision{};
    };

    struct RuntimeEntityRegistry
    {
        std::shared_ptr<const ImportedQuestDatabase> questDefinitions;
        std::unordered_map<NetworkEntityId, RuntimeEntityState> entities;
        std::unordered_map<CanonicalRecordKey, NetworkEntityId, CanonicalRecordKeyHash> sourceToNetwork;
        std::unordered_map<CanonicalRecordKey, std::vector<NetworkEntityId>, CanonicalRecordKeyHash> interiorCells;
        std::unordered_map<ExteriorInterestBucketKey, std::vector<NetworkEntityId>, ExteriorInterestBucketKeyHash> exteriorBuckets;
        NetworkEntityId nextDynamicId{ 1ull << 63 };
        std::uint64_t staticEntities{};
        std::uint64_t actors{};
        std::uint64_t references{};
        std::uint64_t spawned{};
        std::uint64_t despawned{};
        std::uint64_t updates{};
        std::uint64_t rebuckets{};
    };

    RuntimeEntityRegistry BuildRuntimeEntityRegistry(
        const WorldReferenceDatabase& a_world,
        const WorldSpatialContextDatabase& a_spatial);

    NetworkEntityId SpawnRuntimeEntity(
        RuntimeEntityRegistry& a_registry,
        RuntimeEntityKind a_kind,
        const WorldTransform& a_transform,
        const RuntimeEntityLocation& a_location);

    bool DespawnRuntimeEntity(RuntimeEntityRegistry& a_registry, NetworkEntityId a_id);

    bool UpdateRuntimeEntity(
        RuntimeEntityRegistry& a_registry,
        NetworkEntityId a_id,
        const WorldTransform& a_transform,
        const RuntimeEntityLocation& a_location);

    bool UpdateRuntimeActorState(
        RuntimeEntityRegistry& a_registry,
        NetworkEntityId a_id,
        float a_health,
        float a_magicka,
        float a_stamina,
        bool a_dead,
        bool a_inCombat);

    bool UpdateRuntimeStatusState(
        RuntimeEntityRegistry& a_registry,
        NetworkEntityId a_id,
        bool a_dead,
        bool a_inCombat);

    bool UpdateRuntimeEquipmentState(
        RuntimeEntityRegistry& a_registry,
        NetworkEntityId a_id,
        const std::vector<std::uint32_t>& a_equippedFormIds);
}
