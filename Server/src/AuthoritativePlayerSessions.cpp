#include "SessionProtocol.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace SkyrimMP::Server
{
    namespace
    {
        template <class T>
        void AppendIntegral(std::vector<std::uint8_t>& out, T value)
        {
            using U = std::make_unsigned_t<T>;
            const U u = static_cast<U>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                out.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFFu));
            }
        }

        void AppendFloat(std::vector<std::uint8_t>& out, float value)
        {
            AppendIntegral(out, std::bit_cast<std::uint32_t>(value));
        }

        template <class T>
        T ReadIntegral(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("authoritative session payload truncated");
            using U = std::make_unsigned_t<T>;
            U value{};
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value |= static_cast<U>(bytes[offset++]) << (i * 8);
            }
            return static_cast<T>(value);
        }

        float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            return std::bit_cast<float>(ReadIntegral<std::uint32_t>(bytes, offset));
        }

        std::string ReadString8(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            const auto length = ReadIntegral<std::uint8_t>(bytes, offset);
            if (offset + length > bytes.size()) throw std::runtime_error("authoritative session string truncated");
            std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
            return value;
        }

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalRecordKey& key)
        {
            AppendIntegral(out, static_cast<std::uint8_t>(key.kind == FormNamespaceKind::Light ? 1u : 0u));
            AppendIntegral(out, key.namespaceIndex);
            AppendIntegral(out, key.localId);
        }

        CanonicalRecordKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            CanonicalRecordKey key;
            const auto kind = ReadIntegral<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("authoritative player location namespace kind invalid");
            key.kind = kind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
            key.namespaceIndex = ReadIntegral<std::uint32_t>(bytes, offset);
            key.localId = ReadIntegral<std::uint32_t>(bytes, offset);
            return key;
        }

        SessionControlKind ReadKind(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            const auto kind = ReadIntegral<std::uint8_t>(bytes, offset);
            if (kind < static_cast<std::uint8_t>(SessionControlKind::Hello) ||
                kind > static_cast<std::uint8_t>(SessionControlKind::WorldBootstrap)) {
                throw std::runtime_error("authoritative session control kind invalid");
            }
            return static_cast<SessionControlKind>(kind);
        }

        void EnsureConsumed(const std::vector<std::uint8_t>& bytes, std::size_t offset)
        {
            if (offset != bytes.size()) throw std::runtime_error("authoritative session payload trailing bytes");
        }

        std::vector<std::uint8_t> EncodeWelcome(std::uint64_t sessionId, std::uint64_t nonce, std::uint32_t maxPlayers)
        {
            std::vector<std::uint8_t> out;
            AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::Welcome));
            AppendIntegral(out, kReplicationProtocolVersion);
            AppendIntegral(out, sessionId);
            AppendIntegral(out, nonce);
            AppendIntegral(out, maxPlayers);
            return out;
        }

        ClientInterestSubscription DecodePlayerStateRequest(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            ClientInterestSubscription requested;
            const auto flags = ReadIntegral<std::uint8_t>(bytes, offset);
            requested.location.exterior = (flags & 0x01) != 0;
            requested.location.hasCell = (flags & 0x02) != 0;
            requested.location.hasWorldspace = (flags & 0x04) != 0;
            requested.location.cell = ReadKey(bytes, offset);
            requested.location.worldspace = ReadKey(bytes, offset);
            requested.transform.x = ReadFloat(bytes, offset);
            requested.transform.y = ReadFloat(bytes, offset);
            requested.transform.z = ReadFloat(bytes, offset);
            requested.transform.pitch = ReadFloat(bytes, offset);
            requested.transform.yaw = ReadFloat(bytes, offset);
            requested.transform.roll = ReadFloat(bytes, offset);
            requested.health = ReadFloat(bytes, offset);
            requested.magicka = ReadFloat(bytes, offset);
            requested.stamina = ReadFloat(bytes, offset);
            const auto actorFlags = ReadIntegral<std::uint8_t>(bytes, offset);
            if ((actorFlags & ~0x0Fu) != 0) throw std::runtime_error("authoritative player actor flags invalid");
            requested.dead = (actorFlags & 0x01) != 0;
            requested.inCombat = (actorFlags & 0x02) != 0;
            requested.hasActorState = (actorFlags & 0x04) != 0;
            requested.hasStatusState = (actorFlags & 0x08) != 0;
            requested.exteriorRadiusCells = ReadIntegral<std::int32_t>(bytes, offset);

            if (!requested.location.hasCell) throw std::runtime_error("authoritative player request missing CELL");
            if (requested.location.exterior && !requested.location.hasWorldspace) {
                throw std::runtime_error("authoritative exterior player request missing WRLD");
            }
            if (requested.exteriorRadiusCells < 0 || requested.exteriorRadiusCells > 8) {
                throw std::runtime_error("authoritative player request radius invalid");
            }
            return requested;
        }

        bool FiniteTransform(const WorldTransform& transform)
        {
            return std::isfinite(transform.x) && std::isfinite(transform.y) && std::isfinite(transform.z) &&
                std::isfinite(transform.pitch) && std::isfinite(transform.yaw) && std::isfinite(transform.roll);
        }

        bool ValidActorState(const ClientInterestSubscription& requested)
        {
            return std::isfinite(requested.health) && std::isfinite(requested.magicka) && std::isfinite(requested.stamina) &&
                requested.health >= 0.0f && requested.magicka >= 0.0f && requested.stamina >= 0.0f &&
                requested.health <= 1000000.0f && requested.magicka <= 1000000.0f && requested.stamina <= 1000000.0f;
        }

        bool SameLocationContext(const RuntimeEntityLocation& a, const RuntimeEntityLocation& b)
        {
            return a.exterior == b.exterior && a.hasCell == b.hasCell && a.hasWorldspace == b.hasWorldspace &&
                a.cell == b.cell && a.worldspace == b.worldspace;
        }

        bool ReasonableRequestedMove(const RuntimeEntityState& current, const ClientInterestSubscription& requested)
        {
            if (!FiniteTransform(requested.transform)) return false;

            constexpr double kMaxCoordinateMagnitude = 10000000.0;
            if (std::abs(static_cast<double>(requested.transform.x)) > kMaxCoordinateMagnitude ||
                std::abs(static_cast<double>(requested.transform.y)) > kMaxCoordinateMagnitude ||
                std::abs(static_cast<double>(requested.transform.z)) > kMaxCoordinateMagnitude) {
                return false;
            }

            if (!SameLocationContext(current.location, requested.location)) return true;

            constexpr double kMaxStep = 16384.0;
            const auto dx = static_cast<double>(requested.transform.x) - current.transform.x;
            const auto dy = static_cast<double>(requested.transform.y) - current.transform.y;
            const auto dz = static_cast<double>(requested.transform.z) - current.transform.z;
            return dx * dx + dy * dy + dz * dz <= kMaxStep * kMaxStep;
        }

        ClientInterestSubscription InterestFromPlayer(const RuntimeEntityState& player)
        {
            ClientInterestSubscription interest;
            interest.location = player.location;
            interest.transform = player.transform;
            interest.exteriorRadiusCells = 1;
            return interest;
        }

        const RuntimeEntityState* FindRiverwoodSpawn(const RuntimeEntityRegistry& registry)
        {
            // Skyrim.esm's persistent Riverwood map marker. Moving to this
            // reference loads the correct Tamriel exterior cell on the client.
            const CanonicalRecordKey riverwoodMarker{
                FormNamespaceKind::Full,
                0,
                0x000162A4u
            };
            const auto networkIt = registry.sourceToNetwork.find(riverwoodMarker);
            if (networkIt == registry.sourceToNetwork.end()) return nullptr;
            const auto entityIt = registry.entities.find(networkIt->second);
            return entityIt == registry.entities.end() ? nullptr : &entityIt->second;
        }

        CanonicalRecordKey FindTransferAnchor(
            const RuntimeEntityRegistry& registry,
            const RuntimeEntityLocation& location,
            const WorldTransform& transform)
        {
            const RuntimeEntityState* best = nullptr;
            double bestDistance = std::numeric_limits<double>::max();
            for (const auto& [id, entity] : registry.entities) {
                (void)id;
                if (!entity.hasSourceRecord || entity.kind != RuntimeEntityKind::StaticReference) continue;
                if (entity.location.exterior != location.exterior) continue;
                if (location.exterior) {
                    if (!entity.location.hasWorldspace || entity.location.worldspace != location.worldspace) continue;
                } else if (!entity.location.hasCell || entity.location.cell != location.cell) {
                    continue;
                }
                const auto dx = static_cast<double>(entity.transform.x) - transform.x;
                const auto dy = static_cast<double>(entity.transform.y) - transform.y;
                const auto dz = static_cast<double>(entity.transform.z) - transform.z;
                const auto distance = dx * dx + dy * dy + dz * dz;
                if (distance < bestDistance) {
                    best = &entity;
                    bestDistance = distance;
                }
            }
            return best ? best->sourceRecord : CanonicalRecordKey{};
        }

        std::vector<std::uint8_t> EncodeWorldBootstrap(
            std::uint64_t sessionId,
            NetworkEntityId playerEntityId,
            const CanonicalRecordKey& anchor,
            const RuntimeEntityState& player)
        {
            std::vector<std::uint8_t> out;
            AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::WorldBootstrap));
            AppendIntegral(out, sessionId);
            AppendIntegral(out, playerEntityId);
            AppendKey(out, anchor);

            std::uint8_t flags = 0;
            if (player.location.exterior) flags |= 0x01;
            if (player.location.hasCell) flags |= 0x02;
            if (player.location.hasWorldspace) flags |= 0x04;
            AppendIntegral(out, flags);
            AppendKey(out, player.location.cell);
            AppendKey(out, player.location.worldspace);
            AppendFloat(out, player.transform.x);
            AppendFloat(out, player.transform.y);
            AppendFloat(out, player.transform.z);
            AppendFloat(out, player.transform.pitch);
            AppendFloat(out, player.transform.yaw);
            AppendFloat(out, player.transform.roll);
            return out;
        }
    }

    std::optional<ClientInterestSubscription> ServerSessionManager::LoadPersistedPlayer(std::uint64_t characterId) const
    {
        if (characterId == 0 || playerStateDirectory_.empty()) return std::nullopt;
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0') << characterId << ".state";
        std::ifstream input(std::filesystem::path(playerStateDirectory_) / name.str());
        if (!input) return std::nullopt;

        unsigned version{}, exterior{}, hasCell{}, hasWorld{}, cellKind{}, worldKind{}, dead{}, inCombat{}, hasActor{}, hasStatus{};
        ClientInterestSubscription state;
        if (!(input >> version >> exterior >> hasCell >> hasWorld
                >> cellKind >> state.location.cell.namespaceIndex >> state.location.cell.localId
                >> worldKind >> state.location.worldspace.namespaceIndex >> state.location.worldspace.localId
                >> state.transform.x >> state.transform.y >> state.transform.z
                >> state.transform.pitch >> state.transform.yaw >> state.transform.roll
                >> state.health >> state.magicka >> state.stamina >> dead >> inCombat >> hasActor) ||
            version != 1 || exterior > 1 || hasCell > 1 || hasWorld > 1 || cellKind > 1 || worldKind > 1 ||
            dead > 1 || inCombat > 1 || hasActor > 1) {
            throw std::runtime_error("persisted player state is malformed");
        }
        state.location.exterior = exterior != 0;
        state.location.hasCell = hasCell != 0;
        state.location.hasWorldspace = hasWorld != 0;
        state.location.cell.kind = cellKind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
        state.location.worldspace.kind = worldKind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
        state.dead = dead != 0;
        state.inCombat = inCombat != 0;
        state.hasActorState = hasActor != 0;
        if (!(input >> hasStatus)) hasStatus = hasActor;
        if (hasStatus > 1) throw std::runtime_error("persisted player status flag is malformed");
        state.hasStatusState = hasStatus != 0;
        state.exteriorRadiusCells = 1;
        if (!state.location.hasCell || (state.location.exterior && !state.location.hasWorldspace) ||
            !FiniteTransform(state.transform) || (state.hasActorState && !ValidActorState(state))) {
            throw std::runtime_error("persisted player state failed validation");
        }
        return state;
    }

    void ServerSessionManager::PersistPlayer(std::uint64_t characterId, const RuntimeEntityState& player)
    {
        if (characterId == 0 || playerStateDirectory_.empty()) return;
        std::filesystem::create_directories(playerStateDirectory_);
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0') << characterId << ".state";
        const auto path = std::filesystem::path(playerStateDirectory_) / name.str();
        const auto temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("failed to open player-state temporary file");
            output << std::setprecision(9)
                   << 1 << ' ' << player.location.exterior << ' ' << player.location.hasCell << ' ' << player.location.hasWorldspace << ' '
                   << (player.location.cell.kind == FormNamespaceKind::Light) << ' ' << player.location.cell.namespaceIndex << ' ' << player.location.cell.localId << ' '
                   << (player.location.worldspace.kind == FormNamespaceKind::Light) << ' ' << player.location.worldspace.namespaceIndex << ' ' << player.location.worldspace.localId << ' '
                   << player.transform.x << ' ' << player.transform.y << ' ' << player.transform.z << ' '
                   << player.transform.pitch << ' ' << player.transform.yaw << ' ' << player.transform.roll << ' '
                   << player.health << ' ' << player.magicka << ' ' << player.stamina << ' '
                   << player.dead << ' ' << player.inCombat << ' ' << player.hasActorState << ' ' << player.hasStatusState << '\n';
            if (!output) throw std::runtime_error("failed to write player-state temporary file");
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) throw std::runtime_error("failed to commit persisted player state: " + error.message());
        ++stats_.playerStateSaves;
    }

    void ServerSessionManager::ProcessAuthoritativeControlPackets(NetworkTransport& transport, RuntimeEntityRegistry& registry)
    {
        for (const auto& incoming : transport.DrainControlPackets()) {
            try {
                std::size_t offset = 0;
                const auto kind = ReadKind(incoming.payload, offset);

                if (kind == SessionControlKind::Hello) {
                    ++stats_.hellos;
                    const auto protocol = ReadIntegral<std::uint16_t>(incoming.payload, offset);
                    const auto revision = ReadString8(incoming.payload, offset);
                    const auto nonce = ReadIntegral<std::uint64_t>(incoming.payload, offset);
                    EnsureConsumed(incoming.payload, offset);

                    if (protocol != kReplicationProtocolVersion) {
                        Reject(transport, incoming.endpoint, SessionRejectReason::ProtocolMismatch);
                        continue;
                    }
                    if (revision != loadOrderRevision_) {
                        Reject(transport, incoming.endpoint, SessionRejectReason::LoadOrderMismatch);
                        continue;
                    }
                    if (!sessions_.contains(incoming.endpoint) && sessions_.size() >= maxPlayers_) {
                        Reject(transport, incoming.endpoint, SessionRejectReason::ServerFull);
                        continue;
                    }
                    if (CharacterAlreadyOnline(incoming.endpoint, nonce)) {
                        Reject(transport, incoming.endpoint, SessionRejectReason::CharacterAlreadyOnline);
                        continue;
                    }

                    auto [it, inserted] = sessions_.try_emplace(incoming.endpoint);
                    if (inserted) {
                        it->second.sessionId = nextSessionId_++;
                        it->second.endpoint = incoming.endpoint;
                        it->second.clientNonce = nonce;
                        it->second.firstLogin = !seenClientNonces_.contains(nonce);
                        ++stats_.accepted;
                    }
                    it->second.lastHeartbeat = std::chrono::steady_clock::now();
                    transport.SendControl(incoming.endpoint, WireChannel::Reliable,
                        EncodeWelcome(it->second.sessionId, nonce, maxPlayers_));
                    continue;
                }

                const auto sessionIt = sessions_.find(incoming.endpoint);
                if (sessionIt == sessions_.end()) {
                    Reject(transport, incoming.endpoint, SessionRejectReason::InvalidSession);
                    continue;
                }
                auto& session = sessionIt->second;
                const auto sessionId = ReadIntegral<std::uint64_t>(incoming.payload, offset);
                if (sessionId != session.sessionId) {
                    Reject(transport, incoming.endpoint, SessionRejectReason::InvalidSession);
                    continue;
                }

                if (kind == SessionControlKind::Heartbeat) {
                    EnsureConsumed(incoming.payload, offset);
                    session.lastHeartbeat = std::chrono::steady_clock::now();
                    ++stats_.heartbeats;
                    continue;
                }

                if (kind == SessionControlKind::Disconnect) {
                    EnsureConsumed(incoming.payload, offset);
                    if (session.hasPlayerEntity) {
                        const auto playerIt = registry.entities.find(session.playerEntityId);
                        if (playerIt != registry.entities.end() && session.playerStateDirty) PersistPlayer(session.clientNonce, playerIt->second);
                        if (!DespawnRuntimeEntity(registry, session.playerEntityId)) {
                            throw std::runtime_error("authoritative player entity missing during disconnect");
                        }
                        ++stats_.playerEntitiesDespawned;
                    }
                    sessions_.erase(sessionIt);
                    ++stats_.disconnects;
                    continue;
                }

                if (kind == SessionControlKind::BootstrapRequest) {
                    const auto requested = DecodePlayerStateRequest(incoming.payload, offset);
                    EnsureConsumed(incoming.payload, offset);
                    session.lastHeartbeat = std::chrono::steady_clock::now();
                    ++stats_.bootstrapRequests;

                    if (!session.hasPlayerEntity) {
                        const RuntimeEntityState* riverwood = nullptr;
                        std::optional<ClientInterestSubscription> persisted;
                        if (session.firstLogin) {
                            riverwood = FindRiverwoodSpawn(registry);
                            if (riverwood == nullptr) {
                                throw std::runtime_error("Riverwood first-login marker missing from runtime registry");
                            }
                        } else {
                            persisted = LoadPersistedPlayer(session.clientNonce);
                            if (persisted) ++stats_.playerStateRestores;
                        }

                        const auto& spawnTransform = riverwood ? riverwood->transform :
                            (persisted ? persisted->transform : requested.transform);
                        const auto& spawnLocation = riverwood ? riverwood->location :
                            (persisted ? persisted->location : requested.location);
                        const auto& actorState = persisted ? *persisted : requested;
                        if (!FiniteTransform(spawnTransform) || (actorState.hasActorState && !ValidActorState(actorState))) {
                            ++stats_.playerStateRejected;
                            continue;
                        }
                        session.playerEntityId = SpawnRuntimeEntity(
                            registry,
                            RuntimeEntityKind::Player,
                            spawnTransform,
                            spawnLocation);
                        session.hasPlayerEntity = true;
                        if (actorState.hasActorState && !UpdateRuntimeActorState(registry, session.playerEntityId, actorState.health, actorState.magicka,
                                actorState.stamina, actorState.dead, actorState.inCombat)) {
                            throw std::runtime_error("authoritative player initial actor state failed");
                        }
                        if (!actorState.hasActorState && actorState.hasStatusState &&
                            !UpdateRuntimeStatusState(registry, session.playerEntityId, actorState.dead, actorState.inCombat)) {
                            throw std::runtime_error("authoritative player initial status state failed");
                        }
                        session.bootstrapAnchor = riverwood ? riverwood->sourceRecord :
                            (persisted && !SameLocationContext(persisted->location, requested.location) ?
                                FindTransferAnchor(registry, persisted->location, persisted->transform) : CanonicalRecordKey{});
                        if (persisted && !SameLocationContext(persisted->location, requested.location) && session.bootstrapAnchor.localId == 0) {
                            throw std::runtime_error("persisted player destination has no transferable world anchor");
                        }
                        session.hasBootstrapAnchor = true;
                        session.replication.excludedEntityId = session.playerEntityId;
                        session.replication.hasExcludedEntity = true;
                        ++stats_.playerEntitiesSpawned;
                        if (riverwood) {
                            ++stats_.riverwoodFirstLogins;
                        }
                    }

                    const auto playerIt = registry.entities.find(session.playerEntityId);
                    if (playerIt == registry.entities.end()) {
                        throw std::runtime_error("authoritative bootstrap player entity missing");
                    }
                    if (!session.hasBootstrapAnchor) {
                        throw std::runtime_error("authoritative bootstrap anchor missing");
                    }

                    session.interest = InterestFromPlayer(playerIt->second);
                    session.hasInterest = true;
                    transport.SendControl(
                        incoming.endpoint,
                        WireChannel::Reliable,
                        EncodeWorldBootstrap(
                            session.sessionId,
                            session.playerEntityId,
                            session.bootstrapAnchor,
                            playerIt->second));
                    ++stats_.bootstrapAssignments;
                    continue;
                }

                if (kind != SessionControlKind::Interest) {
                    Reject(transport, incoming.endpoint, SessionRejectReason::Malformed);
                    continue;
                }

                ++stats_.interestUpdates;
                ++stats_.playerStateRequests;
                const auto requested = DecodePlayerStateRequest(incoming.payload, offset);
                EnsureConsumed(incoming.payload, offset);
                session.lastHeartbeat = std::chrono::steady_clock::now();

                // Protocol v4 creates the authoritative player during BootstrapRequest;
                // subsequent state reports may only update that server-owned entity.
                if (!session.hasPlayerEntity || !FiniteTransform(requested.transform) ||
                    (requested.hasActorState && !ValidActorState(requested))) {
                    ++stats_.playerStateRejected;
                    continue;
                }

                const auto playerIt = registry.entities.find(session.playerEntityId);
                if (playerIt == registry.entities.end()) {
                    throw std::runtime_error("authoritative player entity missing during update");
                }
                if (!ReasonableRequestedMove(playerIt->second, requested)) {
                    ++stats_.playerStateRejected;
                    continue;
                }
                if (!UpdateRuntimeEntity(registry, session.playerEntityId, requested.transform, requested.location)) {
                    throw std::runtime_error("authoritative player update failed");
                }
                if (requested.hasActorState && !UpdateRuntimeActorState(registry, session.playerEntityId, requested.health, requested.magicka,
                        requested.stamina, requested.dead, requested.inCombat)) {
                    throw std::runtime_error("authoritative player actor-state update failed");
                }
                if (!requested.hasActorState && requested.hasStatusState &&
                    !UpdateRuntimeStatusState(registry, session.playerEntityId, requested.dead, requested.inCombat)) {
                    throw std::runtime_error("authoritative player status-state update failed");
                }
                ++stats_.playerStateApplied;

                // The first accepted post-bootstrap state proves that the client
                // completed the Riverwood transfer and created its MP branch save.
                // Persist only now so a crash during loading can retry onboarding.
                if (session.firstLogin) {
                    MarkFirstLoginComplete(session.clientNonce);
                    session.firstLogin = false;
                }

                const auto updatedPlayerIt = registry.entities.find(session.playerEntityId);
                if (updatedPlayerIt == registry.entities.end()) throw std::runtime_error("authoritative player missing after apply");
                session.interest = InterestFromPlayer(updatedPlayerIt->second);
                session.hasInterest = true;
                session.playerStateDirty = true;
                const auto now = std::chrono::steady_clock::now();
                if (now - session.lastPlayerStatePersist >= std::chrono::seconds(5)) {
                    PersistPlayer(session.clientNonce, updatedPlayerIt->second);
                    session.playerStateDirty = false;
                    session.lastPlayerStatePersist = now;
                }
            } catch (const std::exception&) {
                Reject(transport, incoming.endpoint, SessionRejectReason::Malformed);
            }
        }
    }

    void ServerSessionManager::ExpireIdleAuthoritative(RuntimeEntityRegistry& registry, std::chrono::milliseconds timeout)
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (now - it->second.lastHeartbeat < timeout) {
                ++it;
                continue;
            }

            if (it->second.hasPlayerEntity) {
                const auto playerIt = registry.entities.find(it->second.playerEntityId);
                if (playerIt != registry.entities.end() && it->second.playerStateDirty) PersistPlayer(it->second.clientNonce, playerIt->second);
                if (!DespawnRuntimeEntity(registry, it->second.playerEntityId)) {
                    throw std::runtime_error("authoritative player entity missing during timeout");
                }
                ++stats_.playerEntitiesDespawned;
            }
            it = sessions_.erase(it);
        }
    }

    void ServerSessionManager::FlushAuthoritativePlayers(const RuntimeEntityRegistry& registry)
    {
        for (auto& [endpoint, session] : sessions_) {
            (void)endpoint;
            if (!session.hasPlayerEntity || !session.playerStateDirty) continue;
            const auto playerIt = registry.entities.find(session.playerEntityId);
            if (playerIt == registry.entities.end()) continue;
            PersistPlayer(session.clientNonce, playerIt->second);
            session.playerStateDirty = false;
            session.lastPlayerStatePersist = std::chrono::steady_clock::now();
        }
    }

    std::size_t ServerSessionManager::ActivePlayerCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& [endpoint, session] : sessions_) {
            (void)endpoint;
            if (session.hasPlayerEntity) ++count;
        }
        return count;
    }
}
