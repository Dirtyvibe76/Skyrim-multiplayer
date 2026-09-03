#include "NetworkTransport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uintptr_t kInvalidSocketValue = static_cast<std::uintptr_t>(INVALID_SOCKET);

        SOCKET ToSocket(std::uintptr_t value) { return static_cast<SOCKET>(value); }

        NetworkEndpoint EndpointFromSockaddr(const sockaddr_in& address)
        {
            return NetworkEndpoint{ address.sin_addr.s_addr, ntohs(address.sin_port) };
        }

        sockaddr_in SockaddrFromEndpoint(const NetworkEndpoint& endpoint)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = endpoint.address;
            address.sin_port = htons(endpoint.port);
            return address;
        }

        ReplicationMessage TestDelta(std::uint64_t revision)
        {
            ReplicationMessage message;
            message.kind = ReplicationMessageKind::Delta;
            message.id = 0x8000000000004242ull;
            message.revision = revision;
            message.reliable = false;
            message.snapshot.id = message.id;
            message.snapshot.kind = RuntimeEntityKind::Player;
            message.snapshot.revision = revision;
            message.snapshot.transform = WorldTransform{ 10.0f, 20.0f, 30.0f, 0.0f, 0.0f, 0.0f };
            message.snapshot.location.hasCell = true;
            message.snapshot.location.cell = CanonicalRecordKey{ FormNamespaceKind::Full, 0, 0x1234 };
            return message;
        }

        ReplicationMessage TestSpawn(std::uint64_t revision)
        {
            auto message = TestDelta(revision);
            message.kind = ReplicationMessageKind::Spawn;
            message.reliable = true;
            return message;
        }

        std::vector<std::uint8_t> ReceiveClientDatagram(SOCKET client)
        {
            std::vector<std::uint8_t> bytes(kMaxUdpDatagramBytes);
            sockaddr_in peer{};
            int peerLength = sizeof(peer);
            const auto count = recvfrom(client, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (count <= 0) throw std::runtime_error("network transport self-test client receive failed");
            bytes.resize(static_cast<std::size_t>(count));
            return bytes;
        }
    }

    std::size_t NetworkEndpointHash::operator()(const NetworkEndpoint& endpoint) const noexcept
    {
        std::size_t seed = std::hash<std::uint32_t>{}(endpoint.address);
        seed ^= std::hash<std::uint16_t>{}(endpoint.port) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }

    NetworkTransport::NetworkTransport()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed for network transport");
        winsockStarted_ = true;
    }

    NetworkTransport::~NetworkTransport()
    {
        Close();
        if (winsockStarted_) WSACleanup();
    }

    void NetworkTransport::Bind(std::uint16_t port)
    {
        if (IsBound()) throw std::runtime_error("network transport already bound");
        const auto socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketValue == INVALID_SOCKET) throw std::runtime_error("failed to create UDP server socket");
        socket_ = static_cast<std::uintptr_t>(socketValue);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(socketValue, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            Close();
            throw std::runtime_error("failed to bind UDP server socket");
        }

        u_long nonBlocking = 1;
        if (ioctlsocket(socketValue, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            Close();
            throw std::runtime_error("failed to set UDP server socket nonblocking");
        }

        sockaddr_in actual{};
        int actualLength = sizeof(actual);
        if (getsockname(socketValue, reinterpret_cast<sockaddr*>(&actual), &actualLength) == SOCKET_ERROR) {
            Close();
            throw std::runtime_error("failed to query bound UDP server port");
        }
        boundPort_ = ntohs(actual.sin_port);
    }

    void NetworkTransport::Close()
    {
        if (IsBound()) closesocket(ToSocket(socket_));
        socket_ = kInvalidSocketValue;
        boundPort_ = 0;
        sessions_.clear();
        controlInbox_.clear();
        ackInbox_.clear();
    }

    bool NetworkTransport::IsBound() const noexcept { return socket_ != kInvalidSocketValue; }
    std::uint16_t NetworkTransport::BoundPort() const noexcept { return boundPort_; }

    NetworkSession& NetworkTransport::TouchSession(const NetworkEndpoint& endpoint)
    {
        const auto now = std::chrono::steady_clock::now();
        auto [it, inserted] = sessions_.try_emplace(endpoint);
        if (inserted) {
            it->second.endpoint = endpoint;
            ++stats_.sessionsCreated;
        }
        it->second.lastSeen = now;
        return it->second;
    }

    void NetworkTransport::SendBytes(const NetworkEndpoint& endpoint, const std::vector<std::uint8_t>& bytes)
    {
        if (!IsBound()) throw std::runtime_error("network transport is not bound");
        const auto address = SockaddrFromEndpoint(endpoint);
        const auto sent = sendto(ToSocket(socket_), reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent != static_cast<int>(bytes.size())) throw std::runtime_error("UDP server send failed");
        ++stats_.datagramsSent;
    }

    void NetworkTransport::SendAck(const NetworkEndpoint& endpoint, std::uint32_t ackSequence)
    {
        auto& session = TouchSession(endpoint);
        WirePacket ack;
        ack.kind = WirePacketKind::Ack;
        ack.channel = WireChannel::Reliable;
        ack.sequence = session.nextSendSequence++;
        ack.ackSequence = ackSequence;
        const auto bytes = SerializeWirePacket(ack);
        SendBytes(endpoint, bytes);
        ++session.packetsSent;
    }

    std::uint32_t NetworkTransport::SendPacket(const NetworkEndpoint& endpoint, WirePacket packet)
    {
        auto& session = TouchSession(endpoint);
        packet.sequence = session.nextSendSequence++;
        const auto bytes = SerializeWirePacket(packet);
        SendBytes(endpoint, bytes);
        ++session.packetsSent;

        if (packet.channel == WireChannel::Reliable && packet.kind != WirePacketKind::Ack) {
            ReliablePendingPacket pending;
            pending.sequence = packet.sequence;
            pending.bytes = bytes;
            pending.lastSent = std::chrono::steady_clock::now();
            pending.sendCount = 1;
            session.reliablePending.emplace(packet.sequence, std::move(pending));
            ++stats_.reliableQueued;
        }
        return packet.sequence;
    }

    std::uint32_t NetworkTransport::SendMessages(const NetworkEndpoint& endpoint, WireChannel channel, const std::vector<ReplicationMessage>& messages)
    {
        WirePacket packet;
        packet.kind = WirePacketKind::Data;
        packet.channel = channel;
        packet.messages = messages;
        return SendPacket(endpoint, std::move(packet));
    }

    std::uint32_t NetworkTransport::SendControl(const NetworkEndpoint& endpoint, WireChannel channel, const std::vector<std::uint8_t>& payload)
    {
        WirePacket packet;
        packet.kind = WirePacketKind::Control;
        packet.channel = channel;
        packet.controlPayload = payload;
        return SendPacket(endpoint, std::move(packet));
    }

    void NetworkTransport::PollOnce()
    {
        if (!IsBound()) throw std::runtime_error("network transport is not bound");

        for (;;) {
            std::vector<std::uint8_t> bytes(kMaxUdpDatagramBytes);
            sockaddr_in peer{};
            int peerLength = sizeof(peer);
            const auto count = recvfrom(ToSocket(socket_), reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (count == SOCKET_ERROR) {
                const auto error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) return;
                // Windows reports ICMP "port unreachable" from a recently closed UDP client as
                // WSAECONNRESET on the next recvfrom(). A dedicated server must survive clients
                // exiting or crashing; the application/session timeout owns cleanup.
                if (error == WSAECONNRESET) continue;
                throw std::runtime_error("UDP server receive failed, WSA error=" + std::to_string(error));
            }
            if (count <= 0) return;

            ++stats_.datagramsReceived;
            bytes.resize(static_cast<std::size_t>(count));
            const auto endpoint = EndpointFromSockaddr(peer);
            try {
                const auto packet = DeserializeWirePacket(bytes);
                auto& session = TouchSession(endpoint);
                ++session.packetsReceived;

                if (packet.kind == WirePacketKind::Ack) {
                    ++session.acksReceived;
                    const auto erased = session.reliablePending.erase(packet.ackSequence);
                    if (erased != 0) {
                        ++stats_.reliableAcked;
                        ackInbox_.push_back(ReceivedAcknowledgement{ endpoint, packet.ackSequence });
                    }
                    continue;
                }

                if (packet.sequence > session.highestReceivedSequence) session.highestReceivedSequence = packet.sequence;
                if (packet.channel == WireChannel::Reliable) SendAck(endpoint, packet.sequence);

                if (packet.kind == WirePacketKind::Control) {
                    controlInbox_.push_back(ReceivedControlPacket{ endpoint, packet.sequence, packet.controlPayload });
                    ++stats_.controlReceived;
                }
            } catch (const std::exception&) {
                ++stats_.malformedDatagrams;
            }
        }
    }

    void NetworkTransport::PumpMaintenance(std::chrono::milliseconds resendAfter, std::chrono::milliseconds sessionTimeout)
    {
        if (!IsBound()) throw std::runtime_error("network transport is not bound");
        const auto now = std::chrono::steady_clock::now();

        for (auto sessionIt = sessions_.begin(); sessionIt != sessions_.end();) {
            auto& session = sessionIt->second;
            if (now - session.lastSeen >= sessionTimeout) {
                sessionIt = sessions_.erase(sessionIt);
                ++stats_.sessionsExpired;
                continue;
            }

            for (auto& [sequence, pending] : session.reliablePending) {
                (void)sequence;
                if (now - pending.lastSent < resendAfter) continue;
                SendBytes(session.endpoint, pending.bytes);
                pending.lastSent = now;
                ++pending.sendCount;
                ++session.retransmits;
                ++stats_.retransmits;
            }
            ++sessionIt;
        }
    }

    std::vector<ReceivedControlPacket> NetworkTransport::DrainControlPackets()
    {
        auto result = std::move(controlInbox_);
        controlInbox_.clear();
        return result;
    }

    std::vector<ReceivedAcknowledgement> NetworkTransport::DrainAcknowledgements()
    {
        auto result = std::move(ackInbox_);
        ackInbox_.clear();
        return result;
    }

    std::optional<NetworkSession> NetworkTransport::GetSession(const NetworkEndpoint& endpoint) const
    {
        const auto it = sessions_.find(endpoint);
        if (it == sessions_.end()) return std::nullopt;
        return it->second;
    }

    std::size_t NetworkTransport::SessionCount() const noexcept { return sessions_.size(); }
    const NetworkTransportStats& NetworkTransport::Stats() const noexcept { return stats_; }

    void RunNetworkTransportSelfTest()
    {
        NetworkTransport transport;
        transport.Bind(0);
        if (!transport.IsBound() || transport.BoundPort() == 0) throw std::runtime_error("network transport self-test bind failed");

        SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client == INVALID_SOCKET) throw std::runtime_error("network transport self-test client socket failed");
        struct ClientGuard { SOCKET value; ~ClientGuard() { if (value != INVALID_SOCKET) closesocket(value); } } clientGuard{ client };

        DWORD timeoutMs = 1000;
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == SOCKET_ERROR) {
            throw std::runtime_error("network transport self-test client timeout setup failed");
        }

        sockaddr_in clientAddress{};
        clientAddress.sin_family = AF_INET;
        clientAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        clientAddress.sin_port = 0;
        if (bind(client, reinterpret_cast<const sockaddr*>(&clientAddress), sizeof(clientAddress)) == SOCKET_ERROR) throw std::runtime_error("network transport self-test client bind failed");
        int clientLength = sizeof(clientAddress);
        if (getsockname(client, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength) == SOCKET_ERROR) throw std::runtime_error("network transport self-test client endpoint query failed");
        const NetworkEndpoint clientEndpoint{ clientAddress.sin_addr.s_addr, ntohs(clientAddress.sin_port) };

        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        serverAddress.sin_port = htons(transport.BoundPort());

        WirePacket hello;
        hello.kind = WirePacketKind::Data;
        hello.channel = WireChannel::Unreliable;
        hello.sequence = 1;
        hello.messages.push_back(TestDelta(1));
        const auto helloBytes = SerializeWirePacket(hello);
        if (sendto(client, reinterpret_cast<const char*>(helloBytes.data()), static_cast<int>(helloBytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) != static_cast<int>(helloBytes.size())) throw std::runtime_error("network transport self-test hello send failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        if (transport.SessionCount() != 1) throw std::runtime_error("network transport self-test did not create session");

        const auto reliableSequence = transport.SendMessages(clientEndpoint, WireChannel::Reliable, { TestSpawn(2) });
        const auto firstReliable = DeserializeWirePacket(ReceiveClientDatagram(client));
        if (firstReliable.sequence != reliableSequence || firstReliable.channel != WireChannel::Reliable) throw std::runtime_error("network transport self-test reliable delivery mismatch");

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport.PumpMaintenance(std::chrono::milliseconds(5), std::chrono::seconds(5));
        const auto retransmitted = DeserializeWirePacket(ReceiveClientDatagram(client));
        if (retransmitted.sequence != reliableSequence) throw std::runtime_error("network transport self-test retransmit sequence mismatch");

        WirePacket ack;
        ack.kind = WirePacketKind::Ack;
        ack.channel = WireChannel::Reliable;
        ack.sequence = 2;
        ack.ackSequence = reliableSequence;
        const auto ackBytes = SerializeWirePacket(ack);
        if (sendto(client, reinterpret_cast<const char*>(ackBytes.data()), static_cast<int>(ackBytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) != static_cast<int>(ackBytes.size())) throw std::runtime_error("network transport self-test ACK send failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();

        const auto session = transport.GetSession(clientEndpoint);
        if (!session || !session->reliablePending.empty()) throw std::runtime_error("network transport self-test ACK did not clear reliable queue");
        const auto acknowledgements = transport.DrainAcknowledgements();
        if (acknowledgements.size() != 1 || acknowledgements.front().sequence != reliableSequence || !(acknowledgements.front().endpoint == clientEndpoint)) {
            throw std::runtime_error("network transport self-test ACK inbox failed");
        }

        WirePacket control;
        control.kind = WirePacketKind::Control;
        control.channel = WireChannel::Reliable;
        control.sequence = 3;
        control.controlPayload = { 0xAA, 0xBB, 0xCC };
        const auto controlBytes = SerializeWirePacket(control);
        if (sendto(client, reinterpret_cast<const char*>(controlBytes.data()), static_cast<int>(controlBytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) != static_cast<int>(controlBytes.size())) throw std::runtime_error("network transport self-test control send failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        const auto controls = transport.DrainControlPackets();
        if (controls.size() != 1 || controls.front().payload != control.controlPayload) throw std::runtime_error("network transport control inbox self-test failed");
        const auto controlAck = DeserializeWirePacket(ReceiveClientDatagram(client));
        if (controlAck.kind != WirePacketKind::Ack || controlAck.ackSequence != control.sequence) throw std::runtime_error("network transport control ACK self-test failed");

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        transport.PumpMaintenance(std::chrono::seconds(5), std::chrono::milliseconds(5));
        if (transport.SessionCount() != 0) throw std::runtime_error("network transport self-test session timeout failed");

        const auto& stats = transport.Stats();
        if (stats.sessionsCreated != 1 || stats.reliableQueued != 1 || stats.reliableAcked != 1 || stats.retransmits < 1 || stats.sessionsExpired != 1 || stats.controlReceived != 1) {
            throw std::runtime_error("network transport self-test statistics invariant failed");
        }

        std::cout << "[TRANSPORT] udp=true nonblocking=true sessions=true reliableQueue=true resend=true ackRemoval=true ackInbox=true controlInbox=true timeout=true\n";
        std::cout << "[TRANSPORT-SELFTEST] bind=true sessionCreate=true reliableSend=true retransmit=true ackClear=true ackEvent=true control=true timeoutExpire=true"
                  << " retransmits=" << stats.retransmits
                  << " datagramsSent=" << stats.datagramsSent
                  << " datagramsReceived=" << stats.datagramsReceived << '\n';
    }
}
