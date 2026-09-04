#include "ReplicationProtocol.h"
#include "WireProtocol.h"
#include "NetworkTransport.h"
#include "SessionProtocol.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace SkyrimMP::Server
{
    namespace
    {
        std::int32_t ToGrid(float coordinate)
        {
            return static_cast<std::int32_t>(std::floor(static_cast<double>(coordinate) / static_cast<double>(kExteriorCellSize)));
        }

        ReplicatedEntitySnapshot Snapshot(const RuntimeEntityState& entity)
        {
            ReplicatedEntitySnapshot snapshot;
            snapshot.id = entity.id;
            snapshot.kind = entity.kind;
            snapshot.revision = entity.revision;
            snapshot.transform = entity.transform;
            snapshot.location = entity.location;
            snapshot.sourceRecord = entity.sourceRecord;
            snapshot.hasSourceRecord = entity.hasSourceRecord;
            snapshot.health = entity.health;
            snapshot.magicka = entity.magicka;
            snapshot.stamina = entity.stamina;
            snapshot.dead = entity.dead;
            snapshot.inCombat = entity.inCombat;
            snapshot.hasActorState = entity.hasActorState;
            snapshot.hasStatusState = entity.hasStatusState;
            snapshot.actionFlags = entity.actionFlags;
            snapshot.equippedFormIds = entity.equippedFormIds;
            return snapshot;
        }

        const RuntimeEntityLocation* FindSmallInteriorLocation(const RuntimeEntityRegistry& registry, WorldTransform& transform)
        {
            const std::vector<NetworkEntityId>* smallest = nullptr;
            for (const auto& [cell, ids] : registry.interiorCells) {
                (void)cell;
                if (!ids.empty() && (smallest == nullptr || ids.size() < smallest->size())) smallest = &ids;
            }
            if (smallest == nullptr) return nullptr;
            const auto it = registry.entities.find(smallest->front());
            if (it == registry.entities.end()) throw std::runtime_error("replication self-test interior entity missing");
            transform = it->second.transform;
            return &it->second.location;
        }

        const RuntimeEntityLocation* FindSmallExteriorLocation(const RuntimeEntityRegistry& registry, WorldTransform& transform)
        {
            const std::vector<NetworkEntityId>* smallest = nullptr;
            for (const auto& [bucket, ids] : registry.exteriorBuckets) {
                (void)bucket;
                if (!ids.empty() && (smallest == nullptr || ids.size() < smallest->size())) smallest = &ids;
            }
            if (smallest == nullptr) return nullptr;
            const auto it = registry.entities.find(smallest->front());
            if (it == registry.entities.end()) throw std::runtime_error("replication self-test exterior entity missing");
            transform = it->second.transform;
            return &it->second.location;
        }

        bool HasMessage(const ReplicationFrame& frame, ReplicationMessageKind kind, NetworkEntityId id)
        {
            return std::any_of(frame.messages.begin(), frame.messages.end(), [&](const ReplicationMessage& message) {
                return message.kind == kind && message.id == id;
            });
        }

        const ReplicationMessage& FindMessage(const ReplicationFrame& frame, ReplicationMessageKind kind, NetworkEntityId id)
        {
            const auto it = std::find_if(frame.messages.begin(), frame.messages.end(), [&](const ReplicationMessage& message) {
                return message.kind == kind && message.id == id;
            });
            if (it == frame.messages.end()) throw std::runtime_error("replication self-test message missing");
            return *it;
        }
    }

    std::vector<NetworkEntityId> CollectRuntimeInterestSet(
        const RuntimeEntityRegistry& registry,
        const ClientInterestSubscription& subscription)
    {
        if (!subscription.location.hasCell) throw std::runtime_error("client interest subscription missing CELL context");
        if (subscription.exteriorRadiusCells < 0) throw std::runtime_error("client exterior interest radius cannot be negative");

        std::vector<NetworkEntityId> result;
        if (!subscription.location.exterior) {
            const auto it = registry.interiorCells.find(subscription.location.cell);
            if (it != registry.interiorCells.end()) result = it->second;
            return result;
        }

        if (!subscription.location.hasWorldspace) throw std::runtime_error("exterior client interest subscription missing WRLD context");
        const auto centerX = ToGrid(subscription.transform.x);
        const auto centerY = ToGrid(subscription.transform.y);
        for (std::int32_t y = centerY - subscription.exteriorRadiusCells; y <= centerY + subscription.exteriorRadiusCells; ++y) {
            for (std::int32_t x = centerX - subscription.exteriorRadiusCells; x <= centerX + subscription.exteriorRadiusCells; ++x) {
                const ExteriorInterestBucketKey key{ subscription.location.worldspace, x, y };
                const auto it = registry.exteriorBuckets.find(key);
                if (it != registry.exteriorBuckets.end()) result.insert(result.end(), it->second.begin(), it->second.end());
            }
        }
        return result;
    }

    void MarkReliableReplicationPending(ClientReplicationState& client, const ReplicationMessage& message)
    {
        if (!message.reliable || message.kind == ReplicationMessageKind::Delta) {
            throw std::runtime_error("only reliable spawn/despawn messages can be marked pending");
        }
        client.pendingReliable[message.id] = PendingReliableEntityState{ message.kind, message.revision };
    }

    void CommitReliableReplicationAck(ClientReplicationState& client, const ReplicationMessage& message)
    {
        if (!message.reliable || message.kind == ReplicationMessageKind::Delta) return;

        const auto pending = client.pendingReliable.find(message.id);
        if (pending == client.pendingReliable.end()) return;
        if (pending->second.kind != message.kind || pending->second.revision != message.revision) return;

        if (message.kind == ReplicationMessageKind::Spawn) {
            client.knownRevisions[message.id] = message.revision;
        } else if (message.kind == ReplicationMessageKind::Despawn) {
            client.knownRevisions.erase(message.id);
            client.lastUnreliableSentRevision.erase(message.id);
        }
        client.pendingReliable.erase(pending);
    }

    ReplicationFrame BuildReplicationFrame(
        const RuntimeEntityRegistry& registry,
        ClientReplicationState& client,
        const ClientInterestSubscription& subscription)
    {
        ReplicationFrame frame;
        frame.sequence = ++client.framesBuilt;

        const auto interest = CollectRuntimeInterestSet(registry, subscription);
        frame.interestEntities = interest.size();
        std::unordered_set<NetworkEntityId> interested;
        interested.reserve(interest.size());

        for (const auto id : interest) {
            if (client.hasExcludedEntity && id == client.excludedEntityId) {
                ++frame.selfExcluded;
                ++client.selfEntitiesExcluded;
                continue;
            }
            if (!interested.insert(id).second) throw std::runtime_error("duplicate NetworkEntityId in runtime interest set");
            const auto entityIt = registry.entities.find(id);
            if (entityIt == registry.entities.end()) throw std::runtime_error("interest set references missing runtime entity");
            const auto& entity = entityIt->second;

            if (client.pendingReliable.contains(id)) continue;

            const auto known = client.knownRevisions.find(id);
            if (known == client.knownRevisions.end()) {
                ReplicationMessage message;
                message.kind = ReplicationMessageKind::Spawn;
                message.id = id;
                message.revision = entity.revision;
                message.reliable = true;
                message.snapshot = Snapshot(entity);
                frame.messages.push_back(std::move(message));
                ++frame.spawns;
                ++client.spawnsSent;
            } else if (entity.revision > known->second) {
                const auto lastSent = client.lastUnreliableSentRevision.find(id);
                if (lastSent == client.lastUnreliableSentRevision.end() || entity.revision > lastSent->second || (client.framesBuilt % 20u) == 0u) {
                    ReplicationMessage message;
                    message.kind = ReplicationMessageKind::Delta;
                    message.id = id;
                    message.revision = entity.revision;
                    message.reliable = false;
                    message.snapshot = Snapshot(entity);
                    frame.messages.push_back(std::move(message));
                    client.lastUnreliableSentRevision[id] = entity.revision;
                    ++frame.deltas;
                    ++client.deltasSent;
                }
            }
        }

        for (const auto& [id, revision] : client.knownRevisions) {
            if (client.hasExcludedEntity && id == client.excludedEntityId) continue;
            if (interested.contains(id) || client.pendingReliable.contains(id)) continue;
            ReplicationMessage message;
            message.kind = ReplicationMessageKind::Despawn;
            message.id = id;
            message.revision = revision;
            message.reliable = true;
            frame.messages.push_back(message);
            ++frame.despawns;
            ++client.despawnsSent;
        }

        if (frame.messages.size() != frame.spawns + frame.deltas + frame.despawns) {
            throw std::runtime_error("replication frame message-count invariant failed");
        }
        return frame;
    }

    void RunReplicationProtocolSelfTest(RuntimeEntityRegistry& registry)
    {
        WorldTransform interiorTransform{};
        WorldTransform exteriorTransform{};
        const auto* interiorLocation = FindSmallInteriorLocation(registry, interiorTransform);
        const auto* exteriorLocation = FindSmallExteriorLocation(registry, exteriorTransform);
        if (interiorLocation == nullptr || exteriorLocation == nullptr) return;

        const auto interior = *interiorLocation;
        const auto exterior = *exteriorLocation;
        const auto testId = SpawnRuntimeEntity(registry, RuntimeEntityKind::Player, interiorTransform, interior);

        ClientReplicationState client;
        ClientInterestSubscription subscription;
        subscription.location = interior;
        subscription.transform = interiorTransform;
        subscription.exteriorRadiusCells = 1;

        const auto spawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(spawnFrame, ReplicationMessageKind::Spawn, testId)) throw std::runtime_error("replication self-test did not emit player spawn");
        const auto& spawn = FindMessage(spawnFrame, ReplicationMessageKind::Spawn, testId);
        MarkReliableReplicationPending(client, spawn);
        const auto suppressedSpawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (HasMessage(suppressedSpawnFrame, ReplicationMessageKind::Spawn, testId)) throw std::runtime_error("replication self-test resent unacked spawn");
        CommitReliableReplicationAck(client, spawn);
        if (!client.knownRevisions.contains(testId)) throw std::runtime_error("replication self-test spawn ACK did not commit known state");

        auto movedInterior = interiorTransform;
        movedInterior.x += 32.0f;
        if (!UpdateRuntimeEntity(registry, testId, movedInterior, interior)) throw std::runtime_error("replication self-test interior update failed");
        subscription.transform = movedInterior;
        const auto deltaFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(deltaFrame, ReplicationMessageKind::Delta, testId)) throw std::runtime_error("replication self-test did not emit player delta");

        if (!UpdateRuntimeEntity(registry, testId, exteriorTransform, exterior)) throw std::runtime_error("replication self-test exterior transition failed");
        const auto despawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(despawnFrame, ReplicationMessageKind::Despawn, testId)) throw std::runtime_error("replication self-test did not emit player despawn after leaving interest");
        const auto& despawn = FindMessage(despawnFrame, ReplicationMessageKind::Despawn, testId);
        MarkReliableReplicationPending(client, despawn);
        const auto suppressedDespawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (HasMessage(suppressedDespawnFrame, ReplicationMessageKind::Despawn, testId)) throw std::runtime_error("replication self-test resent unacked despawn");
        CommitReliableReplicationAck(client, despawn);
        if (client.knownRevisions.contains(testId)) throw std::runtime_error("replication self-test despawn ACK did not clear known state");

        subscription.location = exterior;
        subscription.transform = exteriorTransform;
        const auto respawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(respawnFrame, ReplicationMessageKind::Spawn, testId)) throw std::runtime_error("replication self-test did not emit player respawn in exterior interest");
        const auto& respawn = FindMessage(respawnFrame, ReplicationMessageKind::Spawn, testId);
        MarkReliableReplicationPending(client, respawn);
        CommitReliableReplicationAck(client, respawn);

        if (!DespawnRuntimeEntity(registry, testId)) throw std::runtime_error("replication self-test runtime despawn failed");
        const auto finalFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(finalFrame, ReplicationMessageKind::Despawn, testId)) throw std::runtime_error("replication self-test did not emit final player despawn");
        const auto& finalDespawn = FindMessage(finalFrame, ReplicationMessageKind::Despawn, testId);
        MarkReliableReplicationPending(client, finalDespawn);
        CommitReliableReplicationAck(client, finalDespawn);

        const auto selfId = SpawnRuntimeEntity(registry, RuntimeEntityKind::Player, interiorTransform, interior);
        auto remoteTransform = interiorTransform;
        remoteTransform.x += 64.0f;
        const auto remoteId = SpawnRuntimeEntity(registry, RuntimeEntityKind::Player, remoteTransform, interior);
        ClientReplicationState selfFilteredClient;
        selfFilteredClient.excludedEntityId = selfId;
        selfFilteredClient.hasExcludedEntity = true;
        subscription.location = interior;
        subscription.transform = interiorTransform;
        const auto selfFilteredFrame = BuildReplicationFrame(registry, selfFilteredClient, subscription);
        if (HasMessage(selfFilteredFrame, ReplicationMessageKind::Spawn, selfId) || selfFilteredFrame.selfExcluded != 1) {
            throw std::runtime_error("replication self-test failed local-player exclusion");
        }
        if (!HasMessage(selfFilteredFrame, ReplicationMessageKind::Spawn, remoteId)) {
            throw std::runtime_error("replication self-test failed remote-player visibility");
        }
        const auto& remoteSpawn = FindMessage(selfFilteredFrame, ReplicationMessageKind::Spawn, remoteId);
        if (remoteSpawn.snapshot.kind != RuntimeEntityKind::Player) {
            throw std::runtime_error("replication self-test remote-player kind mismatch");
        }
        if (!DespawnRuntimeEntity(registry, remoteId)) throw std::runtime_error("replication self-test remote-player cleanup failed");
        if (!DespawnRuntimeEntity(registry, selfId)) throw std::runtime_error("replication self-test self-filter entity cleanup failed");

        if (registry.entities.size() != registry.staticEntities) throw std::runtime_error("replication self-test leaked dynamic runtime entity");

        std::cout << "[REPLICATION] protocol=" << kReplicationProtocolVersion
                  << " spawnReliable=true deltaReliable=false despawnReliable=true ackCommit=true deltaRefreshFrames=20 selfExclude=true remotePlayers=true"
                  << " exteriorRadiusCells=" << subscription.exteriorRadiusCells << '\n';
        std::cout << "[REPLICATION-SELFTEST] spawn=true spawnAck=true spawnSuppress=true delta=true interestDespawn=true despawnAck=true despawnSuppress=true interestRespawn=true finalDespawn=true selfExclude=true remotePlayerVisible=true"
                  << " spawnFrame=" << spawnFrame.messages.size()
                  << " deltaFrame=" << deltaFrame.messages.size()
                  << " despawnFrame=" << despawnFrame.messages.size()
                  << " respawnFrame=" << respawnFrame.messages.size()
                  << " finalFrame=" << finalFrame.messages.size()
                  << " selfFiltered=" << selfFilteredFrame.selfExcluded << '\n';

        RunWireProtocolSelfTest();
        RunNetworkTransportSelfTest();
        RunSessionProtocolSelfTest();
        RunSessionSocketSelfTest();
    }
}
