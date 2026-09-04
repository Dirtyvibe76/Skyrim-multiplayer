#pragma once

#include "NetworkTransport.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SkyrimMP::Server
{
    enum class SessionControlKind : std::uint8_t
    {
        Hello = 1,
        Welcome = 2,
        Reject = 3,
        Heartbeat = 4,
        Disconnect = 5,
        Interest = 6,
        BootstrapRequest = 7,
        WorldBootstrap = 8,
        AppearanceProfile = 9
    };

    enum class SessionRejectReason : std::uint8_t
    {
        ProtocolMismatch = 1,
        LoadOrderMismatch = 2,
        ServerFull = 3,
        InvalidSession = 4,
        Malformed = 5,
        CharacterAlreadyOnline = 6
    };

    struct AuthenticatedClientSession
    {
        std::uint64_t sessionId{};
        NetworkEndpoint endpoint;
        std::uint64_t clientNonce{};
        std::chrono::steady_clock::time_point lastHeartbeat{};
        ClientReplicationState replication;
        ClientInterestSubscription interest;
        bool hasInterest{};
        NetworkEntityId playerEntityId{};
        bool hasPlayerEntity{};
        CanonicalRecordKey bootstrapAnchor;
        bool hasBootstrapAnchor{};
        bool firstLogin{};
        bool playerStateDirty{};
        PlayerAppearance appearance;
        bool hasAppearance{};
        std::chrono::steady_clock::time_point lastPlayerStatePersist{};
        std::unordered_map<std::uint32_t, std::vector<ReplicationMessage>> reliableReplicationByPacket;
    };

    struct SessionProtocolStats
    {
        std::uint64_t hellos{};
        std::uint64_t accepted{};
        std::uint64_t rejected{};
        std::uint64_t heartbeats{};
        std::uint64_t disconnects{};
        std::uint64_t interestUpdates{};
        std::uint64_t replicationFrames{};
        std::uint64_t replicationMessages{};
        std::uint64_t reliableReplicationPackets{};
        std::uint64_t unreliableReplicationPackets{};
        std::uint64_t reliableReplicationAcks{};
        std::uint64_t reliableReplicationMessagesAcked{};
        std::uint64_t bootstrapRequests{};
        std::uint64_t bootstrapAssignments{};
        std::uint64_t playerEntitiesSpawned{};
        std::uint64_t playerStateRequests{};
        std::uint64_t playerStateApplied{};
        std::uint64_t playerStateRejected{};
        std::uint64_t playerEntitiesDespawned{};
        std::uint64_t riverwoodFirstLogins{};
        std::uint64_t playerStateRestores{};
        std::uint64_t playerStateSaves{};
        std::uint64_t appearanceProfilesReceived{};
        std::uint64_t appearanceProfilesSent{};
    };

    class ServerSessionManager
    {
    public:
        ServerSessionManager(
            std::string a_loadOrderRevision,
            std::uint32_t a_maxPlayers,
            std::string a_firstLoginLedgerPath = {});

        void ProcessAcknowledgements(NetworkTransport& a_transport);
        void ProcessControlPackets(NetworkTransport& a_transport);
        void ProcessAuthoritativeControlPackets(
            NetworkTransport& a_transport,
            RuntimeEntityRegistry& a_registry);
        void ExpireIdle(std::chrono::milliseconds a_timeout = std::chrono::seconds(15));
        void ExpireIdleAuthoritative(
            RuntimeEntityRegistry& a_registry,
            std::chrono::milliseconds a_timeout = std::chrono::seconds(15));
        void FlushAuthoritativePlayers(const RuntimeEntityRegistry& a_registry);
        void SendReplicationFrame(
            NetworkTransport& a_transport,
            const NetworkEndpoint& a_endpoint,
            const ReplicationFrame& a_frame);
        std::uint64_t ReplicateInterestedClients(
            NetworkTransport& a_transport,
            const RuntimeEntityRegistry& a_registry);

        bool IsAuthenticated(const NetworkEndpoint& a_endpoint) const;
        std::size_t SessionCount() const noexcept;
        std::size_t ActivePlayerCount() const noexcept;
        const SessionProtocolStats& Stats() const noexcept;

    private:
        std::string loadOrderRevision_;
        std::string firstLoginLedgerPath_;
        std::string playerStateDirectory_;
        std::uint32_t maxPlayers_{};
        std::uint64_t nextSessionId_{ 1 };
        std::unordered_map<NetworkEndpoint, AuthenticatedClientSession, NetworkEndpointHash> sessions_;
        std::unordered_set<std::uint64_t> seenClientNonces_;
        SessionProtocolStats stats_;

        void Reject(NetworkTransport& a_transport, const NetworkEndpoint& a_endpoint, SessionRejectReason a_reason);
        bool CharacterAlreadyOnline(const NetworkEndpoint& a_endpoint, std::uint64_t a_characterId) const;
        void MarkFirstLoginComplete(std::uint64_t a_characterId);
        std::optional<ClientInterestSubscription> LoadPersistedPlayer(std::uint64_t a_characterId) const;
        void PersistPlayer(std::uint64_t a_characterId, const RuntimeEntityState& a_player);
    };

    std::vector<std::uint8_t> EncodeSessionHello(
        std::uint16_t a_protocolVersion,
        const std::string& a_loadOrderRevision,
        std::uint64_t a_clientNonce);
    std::vector<std::uint8_t> EncodeSessionHeartbeat(std::uint64_t a_sessionId);
    std::vector<std::uint8_t> EncodeSessionDisconnect(std::uint64_t a_sessionId);
    std::vector<std::uint8_t> EncodeSessionInterest(
        std::uint64_t a_sessionId,
        const ClientInterestSubscription& a_interest);

    void RunSessionProtocolSelfTest();
    void RunSessionSocketSelfTest();
}
