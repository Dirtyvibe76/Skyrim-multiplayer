#pragma once

#include "RuntimeEntityRegistry.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    constexpr std::uint16_t kReplicationProtocolVersion = 9;

    enum class ReplicationMessageKind : std::uint8_t
    {
        Spawn,
        Delta,
        Despawn
    };

    struct ReplicatedEntitySnapshot
    {
        NetworkEntityId id{};
        RuntimeEntityKind kind{ RuntimeEntityKind::StaticReference };
        std::uint64_t revision{};
        WorldTransform transform;
        RuntimeEntityLocation location;
        CanonicalRecordKey sourceRecord;
        bool hasSourceRecord{};
        float health{};
        float magicka{};
        float stamina{};
        bool dead{};
        bool inCombat{};
        bool hasActorState{};
        bool hasStatusState{};
        std::uint16_t actionFlags{};
        std::vector<std::uint32_t> equippedFormIds;
    };

    struct ReplicationMessage
    {
        ReplicationMessageKind kind{ ReplicationMessageKind::Delta };
        NetworkEntityId id{};
        std::uint64_t revision{};
        bool reliable{};
        ReplicatedEntitySnapshot snapshot;
    };

    struct PendingReliableEntityState
    {
        ReplicationMessageKind kind{ ReplicationMessageKind::Spawn };
        std::uint64_t revision{};
    };

    struct ClientInterestSubscription
    {
        RuntimeEntityLocation location;
        WorldTransform transform;
        float health{};
        float magicka{};
        float stamina{};
        bool dead{};
        bool inCombat{};
        bool hasActorState{};
        bool hasStatusState{};
        bool hasEquipmentState{};
        std::uint16_t actionFlags{};
        std::vector<std::uint32_t> equippedFormIds;
        // Profile state is persisted with the character but is intentionally
        // not serialized in the 250 ms Interest message.
        PlayerAppearance appearance;
        std::int32_t exteriorRadiusCells{ 1 };
    };

    struct ClientReplicationState
    {
        std::unordered_map<NetworkEntityId, std::uint64_t> knownRevisions;
        std::unordered_map<NetworkEntityId, PendingReliableEntityState> pendingReliable;
        std::unordered_map<NetworkEntityId, std::uint64_t> lastUnreliableSentRevision;
        NetworkEntityId excludedEntityId{};
        bool hasExcludedEntity{};
        std::uint64_t framesBuilt{};
        std::uint64_t spawnsSent{};
        std::uint64_t deltasSent{};
        std::uint64_t despawnsSent{};
        std::uint64_t selfEntitiesExcluded{};
    };

    struct ReplicationFrame
    {
        std::uint16_t protocolVersion{ kReplicationProtocolVersion };
        std::uint64_t sequence{};
        std::vector<ReplicationMessage> messages;
        std::uint64_t interestEntities{};
        std::uint64_t spawns{};
        std::uint64_t deltas{};
        std::uint64_t despawns{};
        std::uint64_t selfExcluded{};
    };

    std::vector<NetworkEntityId> CollectRuntimeInterestSet(
        const RuntimeEntityRegistry& a_registry,
        const ClientInterestSubscription& a_subscription);

    ReplicationFrame BuildReplicationFrame(
        const RuntimeEntityRegistry& a_registry,
        ClientReplicationState& a_client,
        const ClientInterestSubscription& a_subscription);

    void MarkReliableReplicationPending(
        ClientReplicationState& a_client,
        const ReplicationMessage& a_message);
    void CommitReliableReplicationAck(
        ClientReplicationState& a_client,
        const ReplicationMessage& a_message);

    void RunReplicationProtocolSelfTest(RuntimeEntityRegistry& a_registry);
}
