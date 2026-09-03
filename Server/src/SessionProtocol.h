#pragma once

#include "NetworkTransport.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    enum class SessionControlKind : std::uint8_t
    {
        Hello = 1,
        Welcome = 2,
        Reject = 3,
        Heartbeat = 4,
        Disconnect = 5
    };

    enum class SessionRejectReason : std::uint8_t
    {
        ProtocolMismatch = 1,
        LoadOrderMismatch = 2,
        ServerFull = 3,
        InvalidSession = 4,
        Malformed = 5
    };

    struct AuthenticatedClientSession
    {
        std::uint64_t sessionId{};
        NetworkEndpoint endpoint;
        std::uint64_t clientNonce{};
        std::chrono::steady_clock::time_point lastHeartbeat{};
        ClientReplicationState replication;
    };

    struct SessionProtocolStats
    {
        std::uint64_t hellos{};
        std::uint64_t accepted{};
        std::uint64_t rejected{};
        std::uint64_t heartbeats{};
        std::uint64_t disconnects{};
        std::uint64_t replicationFrames{};
        std::uint64_t reliableReplicationPackets{};
        std::uint64_t unreliableReplicationPackets{};
    };

    class ServerSessionManager
    {
    public:
        ServerSessionManager(std::string a_loadOrderRevision, std::uint32_t a_maxPlayers);

        void ProcessControlPackets(NetworkTransport& a_transport);
        void ExpireIdle(std::chrono::milliseconds a_timeout = std::chrono::seconds(15));
        void SendReplicationFrame(
            NetworkTransport& a_transport,
            const NetworkEndpoint& a_endpoint,
            const ReplicationFrame& a_frame);

        bool IsAuthenticated(const NetworkEndpoint& a_endpoint) const;
        std::size_t SessionCount() const noexcept;
        const SessionProtocolStats& Stats() const noexcept;

    private:
        std::string loadOrderRevision_;
        std::uint32_t maxPlayers_{};
        std::uint64_t nextSessionId_{ 1 };
        std::unordered_map<NetworkEndpoint, AuthenticatedClientSession, NetworkEndpointHash> sessions_;
        SessionProtocolStats stats_;

        void Reject(NetworkTransport& a_transport, const NetworkEndpoint& a_endpoint, SessionRejectReason a_reason);
    };

    std::vector<std::uint8_t> EncodeSessionHello(
        std::uint16_t a_protocolVersion,
        const std::string& a_loadOrderRevision,
        std::uint64_t a_clientNonce);
    std::vector<std::uint8_t> EncodeSessionHeartbeat(std::uint64_t a_sessionId);
    std::vector<std::uint8_t> EncodeSessionDisconnect(std::uint64_t a_sessionId);

    void RunSessionProtocolSelfTest();
}
