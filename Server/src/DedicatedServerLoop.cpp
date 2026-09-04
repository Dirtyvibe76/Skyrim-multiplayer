#include "DedicatedServerLoop.h"
#include "NetworkTransport.h"
#include "SessionProtocol.h"
#include "PartyQuestManager.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace SkyrimMP::Server
{
    namespace
    {
        std::atomic_bool g_stopRequested{ false };

        BOOL WINAPI ConsoleHandler(DWORD type)
        {
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
                g_stopRequested.store(true, std::memory_order_relaxed);
                return TRUE;
            }
            return FALSE;
        }

    }

    void RunDedicatedServerLoop(
        RuntimeEntityRegistry& registry,
        const std::string& loadOrderRevision,
        std::uint16_t port,
        std::uint32_t maxPlayers,
        std::uint32_t tickHz)
    {
        if (loadOrderRevision.empty()) throw std::runtime_error("live server loop requires load-order revision");
        if (port == 0 || maxPlayers == 0 || tickHz == 0 || tickHz > 120) {
            throw std::runtime_error("invalid live server configuration");
        }

        NetworkTransport transport;
        transport.Bind(port);
        ServerSessionManager sessions(loadOrderRevision, maxPlayers, "server-data/first-logins.txt");
        if (!registry.questPrograms) throw std::runtime_error("live server loop requires compiled quest programs");
        PartyQuestManager partyQuests(*registry.questPrograms);
        partyQuests.Load("server-data/party-quests.state");

        g_stopRequested.store(false, std::memory_order_relaxed);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        const auto tickInterval = std::chrono::microseconds(1000000 / tickHz);
        auto nextTick = std::chrono::steady_clock::now();
        auto nextStatus = nextTick + std::chrono::seconds(5);
        std::uint64_t ticks{};
        std::uint64_t replicationPasses{};

        std::cout << "[LIVE] listening=0.0.0.0:" << transport.BoundPort()
                  << " tickHz=" << tickHz
                  << " maxPlayers=" << maxPlayers
                  << " protocol=" << kWireProtocolVersion
                  << " loadOrder=" << loadOrderRevision << '\n';
        std::cout << "[LIVE] authority=server-player-entity interest=derived-from-authoritative-player\n";
        std::cout << "[LIVE] Ctrl+C to stop dedicated server\n";

        while (!g_stopRequested.load(std::memory_order_relaxed)) {
            transport.PollOnce();
            sessions.ProcessAcknowledgements(transport);
            sessions.ProcessAuthoritativeControlPackets(transport, registry);
            transport.PumpMaintenance(std::chrono::milliseconds(100), std::chrono::seconds(30));
            sessions.ExpireIdleAuthoritative(registry, std::chrono::seconds(15));

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextTick) {
                replicationPasses += sessions.ReplicateInterestedClients(transport, registry);
                ++ticks;
                do { nextTick += tickInterval; } while (nextTick <= now);
            }

            if (now >= nextStatus) {
                const auto& t = transport.Stats();
                const auto& s = sessions.Stats();
                std::cout << "[LIVE-STATUS] ticks=" << ticks
                          << " sessions=" << sessions.SessionCount()
                          << " players=" << sessions.ActivePlayerCount()
                          << " playerSpawned=" << s.playerEntitiesSpawned
                          << " riverwoodFirstLogins=" << s.riverwoodFirstLogins
                          << " playerRestores=" << s.playerStateRestores
                          << " playerSaves=" << s.playerStateSaves
                          << " playerRequests=" << s.playerStateRequests
                          << " playerApplied=" << s.playerStateApplied
                          << " playerRejected=" << s.playerStateRejected
                          << " playerDespawned=" << s.playerEntitiesDespawned
                          << " interestUpdates=" << s.interestUpdates
                          << " replicationFrames=" << s.replicationFrames
                          << " replicationMessages=" << s.replicationMessages
                          << " reliablePackets=" << s.reliableReplicationPackets
                          << " reliableAcks=" << s.reliableReplicationAcks
                          << " reliableMessagesAcked=" << s.reliableReplicationMessagesAcked
                          << " unreliablePackets=" << s.unreliableReplicationPackets
                          << " replicationPasses=" << replicationPasses
                          << " sent=" << t.datagramsSent
                          << " received=" << t.datagramsReceived
                          << " retransmits=" << t.retransmits << '\n';
                nextStatus = now + std::chrono::seconds(5);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        sessions.FlushAuthoritativePlayers(registry);
        partyQuests.Save("server-data/party-quests.state");
        std::cout << "[LIVE-STOP] ticks=" << ticks
                  << " sessions=" << sessions.SessionCount()
                  << " players=" << sessions.ActivePlayerCount()
                  << " playerSpawned=" << sessions.Stats().playerEntitiesSpawned
                  << " riverwoodFirstLogins=" << sessions.Stats().riverwoodFirstLogins
                  << " playerRestores=" << sessions.Stats().playerStateRestores
                  << " playerSaves=" << sessions.Stats().playerStateSaves
                  << " playerApplied=" << sessions.Stats().playerStateApplied
                  << " playerRejected=" << sessions.Stats().playerStateRejected
                  << " playerDespawned=" << sessions.Stats().playerEntitiesDespawned
                  << " replicationFrames=" << sessions.Stats().replicationFrames
                  << " replicationMessages=" << sessions.Stats().replicationMessages
                  << " reliablePackets=" << sessions.Stats().reliableReplicationPackets
                  << " reliableAcks=" << sessions.Stats().reliableReplicationAcks
                  << " unreliablePackets=" << sessions.Stats().unreliableReplicationPackets << '\n';
    }
}
