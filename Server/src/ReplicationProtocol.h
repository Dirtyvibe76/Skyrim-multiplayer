#pragma once

#include "RuntimeEntityRegistry.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    constexpr std::uint16_t kReplicationProtocolVersion = 1;

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
    };

    struct ReplicationMessage
    {
        ReplicationMessageKind kind{ ReplicationMessageKind::Delta };
        NetworkEntityId id{};
        std::uint64_t revision{};
        bool reliable{};
        ReplicatedEntitySnapshot snapshot;
    };

    struct ClientInterestSubscription
    {
        RuntimeEntityLocation location;
        WorldTransform transform;
        std::int32_t exteriorRadiusCells{ 1 };
    };

    struct ClientReplicationState
    {
        std::unordered_map<NetworkEntityId, std::uint64_t> knownRevisions;
        std::uint64_t framesBuilt{};
        std::uint64_t spawnsSent{};
        std::uint64_t deltasSent{};
        std::uint64_t despawnsSent{};
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
    };

    std::vector<NetworkEntityId> CollectRuntimeInterestSet(
        const RuntimeEntityRegistry& a_registry,
        const ClientInterestSubscription& a_subscription);

    ReplicationFrame BuildReplicationFrame(
        const RuntimeEntityRegistry& a_registry,
        ClientReplicationState& a_client,
        const ClientInterestSubscription& a_subscription);

    void RunReplicationProtocolSelfTest(RuntimeEntityRegistry& a_registry);
}
