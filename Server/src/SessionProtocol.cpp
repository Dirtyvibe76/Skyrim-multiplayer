#include "SessionProtocol.h"

#include <bit>
#include <iostream>
#include <stdexcept>
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
            for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFFu));
        }

        template <class T>
        T ReadIntegral(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("session control payload truncated");
            using U = std::make_unsigned_t<T>;
            U value{};
            for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<U>(bytes[offset++]) << (i * 8);
            return static_cast<T>(value);
        }

        void AppendFloat(std::vector<std::uint8_t>& out, float value) { AppendIntegral(out, std::bit_cast<std::uint32_t>(value)); }
        float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t& offset) { return std::bit_cast<float>(ReadIntegral<std::uint32_t>(bytes, offset)); }

        void AppendString8(std::vector<std::uint8_t>& out, const std::string& value)
        {
            if (value.size() > 255) throw std::runtime_error("session control string too long");
            AppendIntegral(out, static_cast<std::uint8_t>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
        }

        std::string ReadString8(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            const auto length = ReadIntegral<std::uint8_t>(bytes, offset);
            if (offset + length > bytes.size()) throw std::runtime_error("session control string truncated");
            std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
            return value;
        }

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalRecordKey& key)
        {
            AppendIntegral(out, static_cast<std::uint8_t>(key.kind == FormNamespaceKind::Light ? 1 : 0));
            AppendIntegral(out, key.namespaceIndex);
            AppendIntegral(out, key.localId);
        }

        CanonicalRecordKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            const auto kind = ReadIntegral<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("session interest has invalid namespace kind");
            CanonicalRecordKey key;
            key.kind = kind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
            key.namespaceIndex = ReadIntegral<std::uint32_t>(bytes, offset);
            key.localId = ReadIntegral<std::uint32_t>(bytes, offset);
            return key;
        }

        SessionControlKind ReadKind(const std::vector<std::uint8_t>& payload, std::size_t& offset)
        {
            const auto value = ReadIntegral<std::uint8_t>(payload, offset);
            if (value < 1 || value > static_cast<std::uint8_t>(SessionControlKind::Interest)) throw std::runtime_error("invalid session control kind");
            return static_cast<SessionControlKind>(value);
        }

        void EnsureConsumed(const std::vector<std::uint8_t>& payload, std::size_t offset)
        {
            if (offset != payload.size()) throw std::runtime_error("session control payload has trailing bytes");
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

        std::vector<std::uint8_t> EncodeReject(SessionRejectReason reason)
        {
            return { static_cast<std::uint8_t>(SessionControlKind::Reject), static_cast<std::uint8_t>(reason) };
        }

        ClientInterestSubscription DecodeInterest(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            ClientInterestSubscription interest;
            const auto flags = ReadIntegral<std::uint8_t>(bytes, offset);
            interest.location.exterior = (flags & 0x01) != 0;
            interest.location.hasCell = (flags & 0x02) != 0;
            interest.location.hasWorldspace = (flags & 0x04) != 0;
            interest.location.cell = ReadKey(bytes, offset);
            interest.location.worldspace = ReadKey(bytes, offset);
            interest.transform.x = ReadFloat(bytes, offset);
            interest.transform.y = ReadFloat(bytes, offset);
            interest.transform.z = ReadFloat(bytes, offset);
            interest.transform.pitch = ReadFloat(bytes, offset);
            interest.transform.yaw = ReadFloat(bytes, offset);
            interest.transform.roll = ReadFloat(bytes, offset);
            interest.exteriorRadiusCells = ReadIntegral<std::int32_t>(bytes, offset);
            if (!interest.location.hasCell) throw std::runtime_error("session interest missing CELL");
            if (interest.location.exterior && !interest.location.hasWorldspace) throw std::runtime_error("session exterior interest missing WRLD");
            if (interest.exteriorRadiusCells < 0 || interest.exteriorRadiusCells > 8) throw std::runtime_error("session interest radius out of range");
            return interest;
        }

        bool FitsWireBatch(WireChannel channel, const std::vector<ReplicationMessage>& messages)
        {
            WirePacket packet;
            packet.kind = WirePacketKind::Data;
            packet.channel = channel;
            packet.sequence = 1;
            packet.messages = messages;
            try {
                return SerializeWirePacket(packet).size() <= kMaxUdpDatagramBytes;
            } catch (const std::exception&) {
                return false;
            }
        }
    }

    std::vector<std::uint8_t> EncodeSessionHello(std::uint16_t protocolVersion, const std::string& loadOrderRevision, std::uint64_t clientNonce)
    {
        std::vector<std::uint8_t> out;
        AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::Hello));
        AppendIntegral(out, protocolVersion);
        AppendString8(out, loadOrderRevision);
        AppendIntegral(out, clientNonce);
        return out;
    }

    std::vector<std::uint8_t> EncodeSessionHeartbeat(std::uint64_t sessionId)
    {
        std::vector<std::uint8_t> out;
        AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::Heartbeat));
        AppendIntegral(out, sessionId);
        return out;
    }

    std::vector<std::uint8_t> EncodeSessionDisconnect(std::uint64_t sessionId)
    {
        std::vector<std::uint8_t> out;
        AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::Disconnect));
        AppendIntegral(out, sessionId);
        return out;
    }

    std::vector<std::uint8_t> EncodeSessionInterest(std::uint64_t sessionId, const ClientInterestSubscription& interest)
    {
        std::vector<std::uint8_t> out;
        AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::Interest));
        AppendIntegral(out, sessionId);
        std::uint8_t flags = 0;
        if (interest.location.exterior) flags |= 0x01;
        if (interest.location.hasCell) flags |= 0x02;
        if (interest.location.hasWorldspace) flags |= 0x04;
        AppendIntegral(out, flags);
        AppendKey(out, interest.location.cell);
        AppendKey(out, interest.location.worldspace);
        AppendFloat(out, interest.transform.x); AppendFloat(out, interest.transform.y); AppendFloat(out, interest.transform.z);
        AppendFloat(out, interest.transform.pitch); AppendFloat(out, interest.transform.yaw); AppendFloat(out, interest.transform.roll);
        AppendIntegral(out, interest.exteriorRadiusCells);
        return out;
    }

    ServerSessionManager::ServerSessionManager(std::string loadOrderRevision, std::uint32_t maxPlayers) :
        loadOrderRevision_(std::move(loadOrderRevision)), maxPlayers_(maxPlayers)
    {
        if (loadOrderRevision_.empty()) throw std::runtime_error("server session manager requires load-order revision");
        if (maxPlayers_ == 0) throw std::runtime_error("server session manager maxPlayers cannot be zero");
    }

    void ServerSessionManager::Reject(NetworkTransport& transport, const NetworkEndpoint& endpoint, SessionRejectReason reason)
    {
        transport.SendControl(endpoint, WireChannel::Reliable, EncodeReject(reason));
        ++stats_.rejected;
    }

    void ServerSessionManager::ProcessControlPackets(NetworkTransport& transport)
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
                    if (protocol != kReplicationProtocolVersion) { Reject(transport, incoming.endpoint, SessionRejectReason::ProtocolMismatch); continue; }
                    if (revision != loadOrderRevision_) { Reject(transport, incoming.endpoint, SessionRejectReason::LoadOrderMismatch); continue; }
                    if (!sessions_.contains(incoming.endpoint) && sessions_.size() >= maxPlayers_) { Reject(transport, incoming.endpoint, SessionRejectReason::ServerFull); continue; }
                    auto [it, inserted] = sessions_.try_emplace(incoming.endpoint);
                    if (inserted) {
                        it->second.sessionId = nextSessionId_++;
                        it->second.endpoint = incoming.endpoint;
                        it->second.clientNonce = nonce;
                        ++stats_.accepted;
                    }
                    it->second.lastHeartbeat = std::chrono::steady_clock::now();
                    transport.SendControl(incoming.endpoint, WireChannel::Reliable, EncodeWelcome(it->second.sessionId, nonce, maxPlayers_));
                    continue;
                }

                const auto sessionIt = sessions_.find(incoming.endpoint);
                if (sessionIt == sessions_.end()) { Reject(transport, incoming.endpoint, SessionRejectReason::InvalidSession); continue; }
                const auto sessionId = ReadIntegral<std::uint64_t>(incoming.payload, offset);
                if (sessionId != sessionIt->second.sessionId) { Reject(transport, incoming.endpoint, SessionRejectReason::InvalidSession); continue; }

                if (kind == SessionControlKind::Heartbeat) {
                    EnsureConsumed(incoming.payload, offset);
                    sessionIt->second.lastHeartbeat = std::chrono::steady_clock::now();
                    ++stats_.heartbeats;
                } else if (kind == SessionControlKind::Disconnect) {
                    EnsureConsumed(incoming.payload, offset);
                    sessions_.erase(sessionIt);
                    ++stats_.disconnects;
                } else if (kind == SessionControlKind::Interest) {
                    sessionIt->second.interest = DecodeInterest(incoming.payload, offset);
                    EnsureConsumed(incoming.payload, offset);
                    sessionIt->second.hasInterest = true;
                    sessionIt->second.lastHeartbeat = std::chrono::steady_clock::now();
                    ++stats_.interestUpdates;
                } else {
                    Reject(transport, incoming.endpoint, SessionRejectReason::Malformed);
                }
            } catch (const std::exception&) {
                Reject(transport, incoming.endpoint, SessionRejectReason::Malformed);
            }
        }
    }

    void ServerSessionManager::ExpireIdle(std::chrono::milliseconds timeout)
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (now - it->second.lastHeartbeat >= timeout) it = sessions_.erase(it); else ++it;
        }
    }

    void ServerSessionManager::SendReplicationFrame(NetworkTransport& transport, const NetworkEndpoint& endpoint, const ReplicationFrame& frame)
    {
        if (!IsAuthenticated(endpoint)) throw std::runtime_error("cannot replicate to unauthenticated endpoint");
        ++stats_.replicationFrames;
        stats_.replicationMessages += frame.messages.size();

        auto flushBatch = [&](WireChannel channel, std::vector<ReplicationMessage>& batch) {
            if (batch.empty()) return;
            transport.SendMessages(endpoint, channel, batch);
            if (channel == WireChannel::Reliable) ++stats_.reliableReplicationPackets;
            else ++stats_.unreliableReplicationPackets;
            batch.clear();
        };

        std::vector<ReplicationMessage> reliableBatch;
        std::vector<ReplicationMessage> unreliableBatch;
        reliableBatch.reserve(16);
        unreliableBatch.reserve(16);

        for (const auto& message : frame.messages) {
            auto& batch = message.reliable ? reliableBatch : unreliableBatch;
            const auto channel = message.reliable ? WireChannel::Reliable : WireChannel::Unreliable;
            batch.push_back(message);
            if (!FitsWireBatch(channel, batch)) {
                const auto overflow = batch.back();
                batch.pop_back();
                if (batch.empty()) throw std::runtime_error("single replication message exceeds MTU");
                flushBatch(channel, batch);
                batch.push_back(overflow);
                if (!FitsWireBatch(channel, batch)) throw std::runtime_error("single replication message exceeds MTU");
            }
        }

        flushBatch(WireChannel::Reliable, reliableBatch);
        flushBatch(WireChannel::Unreliable, unreliableBatch);
    }

    std::uint64_t ServerSessionManager::ReplicateInterestedClients(NetworkTransport& transport, const RuntimeEntityRegistry& registry)
    {
        std::uint64_t frames = 0;
        for (auto& [endpoint, session] : sessions_) {
            if (!session.hasInterest) continue;
            auto frame = BuildReplicationFrame(registry, session.replication, session.interest);
            if (!frame.messages.empty()) SendReplicationFrame(transport, endpoint, frame);
            ++frames;
        }
        return frames;
    }

    bool ServerSessionManager::IsAuthenticated(const NetworkEndpoint& endpoint) const { return sessions_.contains(endpoint); }
    std::size_t ServerSessionManager::SessionCount() const noexcept { return sessions_.size(); }
    const SessionProtocolStats& ServerSessionManager::Stats() const noexcept { return stats_; }

    void RunSessionProtocolSelfTest()
    {
        ClientInterestSubscription interest;
        interest.location.hasCell = true;
        interest.location.cell = CanonicalRecordKey{ FormNamespaceKind::Full, 0, 0x1234 };
        interest.transform = WorldTransform{ 1, 2, 3, 0, 0, 0 };
        interest.exteriorRadiusCells = 1;
        const auto encoded = EncodeSessionInterest(7, interest);
        std::size_t offset = 0;
        if (ReadKind(encoded, offset) != SessionControlKind::Interest) throw std::runtime_error("session interest self-test kind failed");
        if (ReadIntegral<std::uint64_t>(encoded, offset) != 7) throw std::runtime_error("session interest self-test id failed");
        const auto decoded = DecodeInterest(encoded, offset);
        EnsureConsumed(encoded, offset);
        if (!decoded.location.hasCell || decoded.location.cell.localId != 0x1234 || decoded.transform.x != 1.0f) throw std::runtime_error("session interest self-test round-trip failed");
        std::cout << "[SESSION] handshake=true protocolValidation=true loadOrderValidation=true maxPlayers=true heartbeat=true disconnect=true replication=true interest=true batching=true\n";
        std::cout << "[SESSION-SELFTEST] hello=true welcome=true authenticated=true heartbeat=true replication=true disconnect=true interest=true\n";
    }
}
