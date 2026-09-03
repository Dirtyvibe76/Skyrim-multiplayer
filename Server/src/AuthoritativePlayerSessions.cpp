#include "SessionProtocol.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>

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
                kind > static_cast<std::uint8_t>(SessionControlKind::Interest)) {
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

            // CELL/WRLD transitions are treated as explicit world transitions for now.
            // Inside one spatial context, reject impossible single-request teleports.
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

                    auto [it, inserted] = sessions_.try_emplace(incoming.endpoint);
                    if (inserted) {
                        it->second.sessionId = nextSessionId_++;
                        it->second.endpoint = incoming.endpoint;
                        it->second.clientNonce = nonce;
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
                        if (!DespawnRuntimeEntity(registry, session.playerEntityId)) {
                            throw std::runtime_error("authoritative player entity missing during disconnect");
                        }
                        ++stats_.playerEntitiesDespawned;
                    }
                    sessions_.erase(sessionIt);
                    ++stats_.disconnects;
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

                if (!FiniteTransform(requested.transform)) {
                    ++stats_.playerStateRejected;
                    continue;
                }

                if (!session.hasPlayerEntity) {
                    session.playerEntityId = SpawnRuntimeEntity(
                        registry,
                        RuntimeEntityKind::Player,
                        requested.transform,
                        requested.location);
                    session.hasPlayerEntity = true;
                    ++stats_.playerEntitiesSpawned;
                    ++stats_.playerStateApplied;
                } else {
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
                    ++stats_.playerStateApplied;
                }

                const auto playerIt = registry.entities.find(session.playerEntityId);
                if (playerIt == registry.entities.end()) throw std::runtime_error("authoritative player missing after apply");
                session.interest = InterestFromPlayer(playerIt->second);
                session.hasInterest = true;
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
                if (!DespawnRuntimeEntity(registry, it->second.playerEntityId)) {
                    throw std::runtime_error("authoritative player entity missing during timeout");
                }
                ++stats_.playerEntitiesDespawned;
            }
            it = sessions_.erase(it);
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
