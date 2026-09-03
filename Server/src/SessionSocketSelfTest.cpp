#include "SessionProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace SkyrimMP::Server
{
    namespace
    {
        template <class T>
        T ReadIntegral(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("session socket self-test payload truncated");
            using U = std::make_unsigned_t<T>;
            U value{};
            for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<U>(bytes[offset++]) << (i * 8);
            return static_cast<T>(value);
        }

        void SendPacket(SOCKET socketValue, const sockaddr_in& server, const WirePacket& packet)
        {
            const auto bytes = SerializeWirePacket(packet);
            const auto sent = sendto(socketValue, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<const sockaddr*>(&server), sizeof(server));
            if (sent != static_cast<int>(bytes.size())) throw std::runtime_error("session socket self-test send failed");
        }

        WirePacket ReceiveKind(SOCKET socketValue, WirePacketKind expected)
        {
            for (int attempt = 0; attempt < 8; ++attempt) {
                std::vector<std::uint8_t> bytes(kMaxUdpDatagramBytes);
                sockaddr_in peer{};
                int peerLength = sizeof(peer);
                const auto count = recvfrom(socketValue, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                    reinterpret_cast<sockaddr*>(&peer), &peerLength);
                if (count <= 0) throw std::runtime_error("session socket self-test receive failed");
                bytes.resize(static_cast<std::size_t>(count));
                auto packet = DeserializeWirePacket(bytes);
                if (packet.kind == expected) return packet;
            }
            throw std::runtime_error("session socket self-test expected packet not received");
        }
    }

    void RunSessionSocketSelfTest()
    {
        constexpr const char* revision = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        constexpr std::uint64_t nonce = 0xAABBCCDDEEFF0011ull;

        NetworkTransport transport;
        transport.Bind(0);
        ServerSessionManager manager(revision, 2);

        SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client == INVALID_SOCKET) throw std::runtime_error("session socket self-test client socket failed");
        struct Guard { SOCKET s; ~Guard() { if (s != INVALID_SOCKET) closesocket(s); } } guard{ client };
        DWORD timeoutMs = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        sockaddr_in clientAddress{};
        clientAddress.sin_family = AF_INET;
        clientAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        clientAddress.sin_port = 0;
        if (bind(client, reinterpret_cast<const sockaddr*>(&clientAddress), sizeof(clientAddress)) == SOCKET_ERROR) throw std::runtime_error("session socket self-test client bind failed");

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        server.sin_port = htons(transport.BoundPort());

        WirePacket hello;
        hello.kind = WirePacketKind::Control;
        hello.channel = WireChannel::Reliable;
        hello.sequence = 1;
        hello.controlPayload = EncodeSessionHello(kReplicationProtocolVersion, revision, nonce);
        SendPacket(client, server, hello);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (manager.SessionCount() != 1) throw std::runtime_error("session socket self-test authentication failed");

        const auto welcome = ReceiveKind(client, WirePacketKind::Control);
        std::size_t offset = 0;
        if (ReadIntegral<std::uint8_t>(welcome.controlPayload, offset) != static_cast<std::uint8_t>(SessionControlKind::Welcome)) throw std::runtime_error("session socket self-test Welcome missing");
        if (ReadIntegral<std::uint16_t>(welcome.controlPayload, offset) != kReplicationProtocolVersion) throw std::runtime_error("session socket self-test protocol mismatch");
        const auto sessionId = ReadIntegral<std::uint64_t>(welcome.controlPayload, offset);
        if (ReadIntegral<std::uint64_t>(welcome.controlPayload, offset) != nonce) throw std::runtime_error("session socket self-test nonce mismatch");

        WirePacket interest;
        interest.kind = WirePacketKind::Control;
        interest.channel = WireChannel::Reliable;
        interest.sequence = 2;
        ClientInterestSubscription subscription;
        subscription.location.hasCell = true;
        subscription.location.cell = CanonicalRecordKey{ FormNamespaceKind::Full, 0, 0x1234 };
        subscription.transform = WorldTransform{ 10, 20, 30, 0, 0, 0 };
        interest.controlPayload = EncodeSessionInterest(sessionId, subscription);
        SendPacket(client, server, interest);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (manager.Stats().interestUpdates != 1) throw std::runtime_error("session socket self-test interest update failed");

        WirePacket heartbeat;
        heartbeat.kind = WirePacketKind::Control;
        heartbeat.channel = WireChannel::Reliable;
        heartbeat.sequence = 3;
        heartbeat.controlPayload = EncodeSessionHeartbeat(sessionId);
        SendPacket(client, server, heartbeat);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (manager.Stats().heartbeats != 1) throw std::runtime_error("session socket self-test heartbeat failed");

        WirePacket disconnect;
        disconnect.kind = WirePacketKind::Control;
        disconnect.channel = WireChannel::Reliable;
        disconnect.sequence = 4;
        disconnect.controlPayload = EncodeSessionDisconnect(sessionId);
        SendPacket(client, server, disconnect);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transport.PollOnce();
        manager.ProcessControlPackets(transport);
        if (manager.SessionCount() != 0 || manager.Stats().disconnects != 1) throw std::runtime_error("session socket self-test disconnect failed");

        std::cout << "[SESSION-SOCKET-SELFTEST] hello=true welcome=true authenticated=true interest=true heartbeat=true disconnect=true sessionId=" << sessionId << '\n';
    }
}
