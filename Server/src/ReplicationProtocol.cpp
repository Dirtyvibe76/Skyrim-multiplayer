#include "ReplicationProtocol.h"

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
            if (!interested.insert(id).second) throw std::runtime_error("duplicate NetworkEntityId in runtime interest set");
            const auto entityIt = registry.entities.find(id);
            if (entityIt == registry.entities.end()) throw std::runtime_error("interest set references missing runtime entity");
            const auto& entity = entityIt->second;

            const auto known = client.knownRevisions.find(id);
            if (known == client.knownRevisions.end()) {
                ReplicationMessage message;
                message.kind = ReplicationMessageKind::Spawn;
                message.id = id;
                message.revision = entity.revision;
                message.reliable = true;
                message.snapshot = Snapshot(entity);
                frame.messages.push_back(std::move(message));
                client.knownRevisions[id] = entity.revision;
                ++frame.spawns;
                ++client.spawnsSent;
            } else if (entity.revision > known->second) {
                ReplicationMessage message;
                message.kind = ReplicationMessageKind::Delta;
                message.id = id;
                message.revision = entity.revision;
                message.reliable = false;
                message.snapshot = Snapshot(entity);
                frame.messages.push_back(std::move(message));
                known->second = entity.revision;
                ++frame.deltas;
                ++client.deltasSent;
            }
        }

        for (auto it = client.knownRevisions.begin(); it != client.knownRevisions.end();) {
            if (interested.contains(it->first)) {
                ++it;
                continue;
            }
            ReplicationMessage message;
            message.kind = ReplicationMessageKind::Despawn;
            message.id = it->first;
            message.revision = it->second;
            message.reliable = true;
            frame.messages.push_back(message);
            ++frame.despawns;
            ++client.despawnsSent;
            it = client.knownRevisions.erase(it);
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
        if (!HasMessage(spawnFrame, ReplicationMessageKind::Spawn, testId)) {
            throw std::runtime_error("replication self-test did not emit player spawn");
        }

        auto movedInterior = interiorTransform;
        movedInterior.x += 32.0f;
        if (!UpdateRuntimeEntity(registry, testId, movedInterior, interior)) throw std::runtime_error("replication self-test interior update failed");
        subscription.transform = movedInterior;
        const auto deltaFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(deltaFrame, ReplicationMessageKind::Delta, testId)) {
            throw std::runtime_error("replication self-test did not emit player delta");
        }

        if (!UpdateRuntimeEntity(registry, testId, exteriorTransform, exterior)) throw std::runtime_error("replication self-test exterior transition failed");
        const auto despawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(despawnFrame, ReplicationMessageKind::Despawn, testId)) {
            throw std::runtime_error("replication self-test did not emit player despawn after leaving interest");
        }

        subscription.location = exterior;
        subscription.transform = exteriorTransform;
        const auto respawnFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(respawnFrame, ReplicationMessageKind::Spawn, testId)) {
            throw std::runtime_error("replication self-test did not emit player respawn in exterior interest");
        }

        if (!DespawnRuntimeEntity(registry, testId)) throw std::runtime_error("replication self-test runtime despawn failed");
        const auto finalFrame = BuildReplicationFrame(registry, client, subscription);
        if (!HasMessage(finalFrame, ReplicationMessageKind::Despawn, testId)) {
            throw std::runtime_error("replication self-test did not emit final player despawn");
        }
        if (registry.entities.size() != registry.staticEntities) throw std::runtime_error("replication self-test leaked dynamic runtime entity");

        std::cout << "[REPLICATION] protocol=" << kReplicationProtocolVersion
                  << " spawnReliable=true deltaReliable=false despawnReliable=true"
                  << " exteriorRadiusCells=" << subscription.exteriorRadiusCells << '\n';
        std::cout << "[REPLICATION-SELFTEST] spawn=true delta=true interestDespawn=true interestRespawn=true finalDespawn=true"
                  << " spawnFrame=" << spawnFrame.messages.size()
                  << " deltaFrame=" << deltaFrame.messages.size()
                  << " despawnFrame=" << despawnFrame.messages.size()
                  << " respawnFrame=" << respawnFrame.messages.size()
                  << " finalFrame=" << finalFrame.messages.size() << '\n';
    }
}
