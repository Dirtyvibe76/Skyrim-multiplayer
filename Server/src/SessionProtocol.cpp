#include "SessionProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace SkyrimMP::Server
{
    namespace
    {
        template <class T>
        void AppendIntegral(std::vector<std::uint8_t>& out, T value)
        {
            using U = std::make_unsigned_t<T>;
            const auto u = static_cast<U>(value);
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
            std::vector<std::uint8_t> out;
            AppendIntegral(out, static_cast<std::uint8_t>(SessionControlKind::Reject));
            AppendIntegral(out, static_cast<std::uint8_t>(reason));
            return out;
        }

        SessionControlKind ReadKind(const std::vector<std::uint8_t>& payload, std::size_t& offset)
        {
            const auto value = ReadIntegral<std::uint8_t>(payload, offset);
            if (value < static_cast<std::uint8_t>(SessionControlKind::Hello) || value > static_cast<std::uint8_t>(SessionControlKind::Disconnect)) {
                throw std::runtime_error("invalid session control kind");
            }
            return static_cast<SessionControlKind>(value);
        }

        void EnsureConsumed(const std::vector<std::uint8_t>& payload, std::size_t offset)
        {
            if (offset != payload.size()) throw std::runtime_error("session control payload has trailing bytes");
        }

        sockaddr_in ServerAddress(std::uint16_t port)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            return address;
        }

        void SendClientPacket(SOCKET client, const sockaddr_in& server, WirePacket packet)
        {
            const auto bytes = SerializeWirePacket(packet);
            const auto sent = sendto(client, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<const sockaddr*>(&server), sizeof(server));
            if (sent != static_cast<int>(bytes.size())) throw std::runtime_error("session self-test client send failed");
        }

        WirePacket ReceiveClientPacket(SOCKET client)
        {
            std::vector<std::uint8_t> bytes(kMaxUdpDatagramBytes);
            sockaddr_in peer{};
            int peerLength = sizeof(peer);
            const auto count = recvfrom(client, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (count <= 0) throw std::runtime_error("session self-test client receive failed");
            bytes.resize(static_cast<std::size_t>(count));
            return DeserializeWirePacket(bytes);
        }

        WirePacket ReceiveUntilKind(SOCKET client, WirePacketKind kind)
        {
            for (int i = 0; i < 4; ++i) {
                auto packet = ReceiveClientPacket(client);
                if (packet.kind == kind) return packet;
            }
            throw std::runtime_error("session self-test did not receive expected packet kind");
        }

        std::uint64_t DecodeWelcomeSessionId(const std::vector<std::uint8_t>& payload, std::uint64_t expectedNonce)
        {
            std::size_t offset = 0;
            if (ReadKind(payload, offset) != SessionControlKind::Welcome) throw std::runtime_error("session self-test expected Welcome");
            if (ReadIntegral<std::uint16_t>(payload, offset) != kReplicationProtocolVersion) throw std::runtime_error("session self-test Welcome protocol mismatch");
            const auto sessionId = ReadIntegral<std::uint64_t>(payload, offset);
            const auto nonce = ReadIntegral<std::uint64_t>(payload, offset);
            (void)ReadIntegral<std::uint32_t>(payload, offset);
            EnsureConsumed(payload, offset);
            if (nonce != expectedNonce) throw std::runtime_error("session self-test Welcome nonce mismatch");
            return sessionId;
        }

        NetworkEndpoint EndpointForSocket(SOCKET socketValue)
        {
            sockaddr_in address{};
            int length = sizeof(address);
            if (getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) throw std::runtime_error("session self-test client endpoint query failed");
            return NetworkEndpoint{ address.sin_addr.s_addr, ntohs(address.sin_port) };
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
                    transport.SendControl(incoming.endpoint, WireChannel::Reliable, EncodeWelcome(it->second.sessionId, nonce, maxPlayers_));
                    continue;
                }

                const auto sessionIt = sessions_.find(incoming.endpoint);
                if (sessionIt == sessions_.end()) {
                    Reject(transport, incoming.endpoint, SessionRejectReason::InvalidSession);
                    continue;
                }
                const auto sessionId = ReadIntegral<std::uint64_t>(incoming.payload, offset);
                EnsureConsumed(incoming.payload, offset);
                if (sessionId != sessionIt->second.sessionId) {
                    Reject(transport, incoming.endpoint, SessionRejectReason::InvalidSession);
                    continue;
                }

                if (kind == SessionControlKind::Heartbeat) {
                    sessionIt->second.lastHeartbeat = std::chrono::steady_clock::now();
                    ++stats_.heartbeats;
                } else if (kind == SessionControlKind::Disconnect) {
                    sessions_.erase(sessionIt);
                    ++stats_.disconnects;
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
            if (now - it->second.lastHeartbeat >= timeout) it = sessions_.erase(it);
            else ++it;
        }
    }

    void ServerSessionManager::SendReplicationFrame(NetworkTransport& transport, const NetworkEndpoint& endpoint, const ReplicationFrame& frame)
    {
        if (!IsAuthenticated(endpoint)) throw std::runtime_error("cannot replicate to unauthenticated endpoint");
        ++stats_.replicationFrames;
        for (const auto& message : frame.messages) {
            transport.SendMessages(endpoint, message.reliable ? WireChannel::Reliable : WireChannel::Unreliable, { message });
            if (message.reliable) ++stats_.reliableReplicationPackets;
            else ++stats_.unreliableReplicationPackets;
        }
    }

    bool ServerSessionManager::IsAuthenticated(const NetworkEndpoint& endpoint) const
    {
        return sessions_.contains(endpoint);
    }

    std::size_t ServerSessionManager::SessionCount() const noexcept
    {
        return sessions_.size();
    }

    const SessionProtocolStats& ServerSessionManager::Stats() const noexcept
    {
        return stats_;
    }

    void RunSessionProtocolSelfTest()
    {
        constexpr auto revision = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        constexpr std::uint64_t nonce = 0x1122334455667788ull;

        NetworkTransport transport;
        transport.Bind(0);
        ServerSessionManager manager(revision, 2);

        SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client == INVALID_SOCKET) throw std::runtime_error("session self-test client socket failed");
        struct ClientGuard { SOCKET value; ~ClientGuard() { if (value != INVALID_SOCKET) closesocket(value); } } guard{ client };
        DWORD timeoutMs = 1000;
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == SOCKET_ERROR) throw std::runtime_error("session self-test client timeout failed");
        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bindAddress.sin_port = 0;
        if (bind(client, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR) throw std::runtime_error("session self-test client bind failed");
        const auto endpoint = EndpointForSocket(client);
        const auto server = ServerAddress(transport.BoundPort());

        WirePacket hello;
        hello.kind = WirePacketKind::Control;
        hello.channel = WireChannel::Reliable;
        hello.sequence = 1;
        hello.controlPayload = EncodeSessionHello(kReplicationProtocolVersion, revision, nonce);
        SendClientPacket(client, server, hello);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (!manager.IsAuthenticated(endpoint) || manager.SessionCount() != 1) throw std::runtime_error("session self-test authentication failed");

        const auto welcomePacket = ReceiveUntilKind(client, WirePacketKind::Control);
        const auto sessionId = DecodeWelcomeSessionId(welcomePacket.controlPayload, nonce);
        WirePacket welcomeAck;
        welcomeAck.kind = WirePacketKind::Ack;
        welcomeAck.channel = WireChannel::Reliable;
        welcomeAck.sequence = 2;
        welcomeAck.ackSequence = welcomePacket.sequence;
        SendClientPacket(client, server, welcomeAck);

        ReplicationMessage delta;
        delta.kind = ReplicationMessageKind::Delta;
        delta.id = 0x8000000000000055ull;
        delta.revision = 2;
        delta.reliable = false;
        delta.snapshot.id = delta.id;
        delta.snapshot.kind = RuntimeEntityKind::Player;
        delta.snapshot.revision = delta.revision;
        delta.snapshot.location.hasCell = true;
        delta.snapshot.location.cell = CanonicalRecordKey{ FormNamespaceKind::Full, 0, 0x1234 };
        ReplicationFrame frame;
        frame.sequence = 1;
        frame.messages.push_back(delta);
        manager.SendReplicationFrame(transport, endpoint, frame);
        const auto dataPacket = ReceiveUntilKind(client, WirePacketKind::Data);
        if (dataPacket.messages.size() != 1 || dataPacket.messages.front().id != delta.id) throw std::runtime_error("session self-test replication delivery failed");

        WirePacket heartbeat;
        heartbeat.kind = WirePacketKind::Control;
        heartbeat.channel = WireChannel::Reliable;
        heartbeat.sequence = 3;
        heartbeat.controlPayload = EncodeSessionHeartbeat(sessionId);
        SendClientPacket(client, server, heartbeat);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (manager.Stats().heartbeats != 1) throw std::runtime_error("session self-test heartbeat failed");

        WirePacket disconnect;
        disconnect.kind = WirePacketKind::Control;
        disconnect.channel = WireChannel::Reliable;
        disconnect.sequence = 4;
        disconnect.controlPayload = EncodeSessionDisconnect(sessionId);
        SendClientPacket(client, server, disconnect);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (manager.SessionCount() != 0 || manager.Stats().disconnects != 1) throw std::runtime_error("session self-test disconnect failed");

        const auto& stats = manager.Stats();
        if (stats.hellos != 1 || stats.accepted != 1 || stats.replicationFrames != 1 || stats.unreliableReplicationPackets != 1) {
            throw std::runtime_error("session self-test statistics invariant failed");
        }

        std::cout << "[SESSION] handshake=true protocolValidation=true loadOrderValidation=true maxPlayers=true heartbeat=true disconnect=true replication=true\n";
        std::cout << "[SESSION-SELFTEST] hello=true welcome=true authenticated=true heartbeat=true replication=true disconnect=true"
                  << " sessionId=" << sessionId
                  << " active=" << manager.SessionCount()
                  << " frames=" << stats.replicationFrames << '\n';
    }
}
