#pragma once

#include "WireProtocol.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sockaddr_in;

namespace SkyrimMP::Server
{
    struct NetworkEndpoint
    {
        std::uint32_t address{}; // network byte order
        std::uint16_t port{};    // host byte order

        bool operator==(const NetworkEndpoint&) const = default;
    };

    struct NetworkEndpointHash
    {
        std::size_t operator()(const NetworkEndpoint& a_endpoint) const noexcept;
    };

    struct ReliablePendingPacket
    {
        std::uint32_t sequence{};
        std::vector<std::uint8_t> bytes;
        std::chrono::steady_clock::time_point lastSent{};
        std::uint32_t sendCount{};
    };

    struct NetworkSession
    {
        NetworkEndpoint endpoint;
        std::uint32_t nextSendSequence{ 1 };
        std::uint32_t highestReceivedSequence{};
        std::chrono::steady_clock::time_point lastSeen{};
        std::unordered_map<std::uint32_t, ReliablePendingPacket> reliablePending;
        std::uint64_t packetsReceived{};
        std::uint64_t packetsSent{};
        std::uint64_t acksReceived{};
        std::uint64_t retransmits{};
    };

    struct ReceivedControlPacket
    {
        NetworkEndpoint endpoint;
        std::uint32_t sequence{};
        std::vector<std::uint8_t> payload;
    };

    struct NetworkTransportStats
    {
        std::uint64_t datagramsReceived{};
        std::uint64_t datagramsSent{};
        std::uint64_t malformedDatagrams{};
        std::uint64_t sessionsCreated{};
        std::uint64_t sessionsExpired{};
        std::uint64_t reliableQueued{};
        std::uint64_t reliableAcked{};
        std::uint64_t retransmits{};
        std::uint64_t controlReceived{};
    };

    class NetworkTransport
    {
    public:
        NetworkTransport();
        ~NetworkTransport();
        NetworkTransport(const NetworkTransport&) = delete;
        NetworkTransport& operator=(const NetworkTransport&) = delete;

        void Bind(std::uint16_t a_port);
        void Close();
        bool IsBound() const noexcept;
        std::uint16_t BoundPort() const noexcept;

        void PollOnce();
        void PumpMaintenance(
            std::chrono::milliseconds a_resendAfter = std::chrono::milliseconds(100),
            std::chrono::milliseconds a_sessionTimeout = std::chrono::seconds(30));

        std::uint32_t SendMessages(
            const NetworkEndpoint& a_endpoint,
            WireChannel a_channel,
            const std::vector<ReplicationMessage>& a_messages);

        std::uint32_t SendControl(
            const NetworkEndpoint& a_endpoint,
            WireChannel a_channel,
            const std::vector<std::uint8_t>& a_payload);

        std::vector<ReceivedControlPacket> DrainControlPackets();
        std::optional<NetworkSession> GetSession(const NetworkEndpoint& a_endpoint) const;
        std::size_t SessionCount() const noexcept;
        const NetworkTransportStats& Stats() const noexcept;

    private:
        std::uintptr_t socket_{ static_cast<std::uintptr_t>(~0ull) };
        std::uint16_t boundPort_{};
        bool winsockStarted_{};
        std::unordered_map<NetworkEndpoint, NetworkSession, NetworkEndpointHash> sessions_;
        std::vector<ReceivedControlPacket> controlInbox_;
        NetworkTransportStats stats_;

        NetworkSession& TouchSession(const NetworkEndpoint& a_endpoint);
        void SendBytes(const NetworkEndpoint& a_endpoint, const std::vector<std::uint8_t>& a_bytes);
        void SendAck(const NetworkEndpoint& a_endpoint, std::uint32_t a_ackSequence);
        std::uint32_t SendPacket(const NetworkEndpoint& a_endpoint, WirePacket a_packet);
    };

    void RunNetworkTransportSelfTest();
}
