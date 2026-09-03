#include "DedicatedServerLoop.h"
#include "NetworkTransport.h"
#include "SessionProtocol.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
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

        struct LiveConfig
        {
            std::uint16_t port{ 10578 };
            std::uint32_t maxPlayers{ 64 };
            std::uint32_t tickHz{ 20 };
        };

        LiveConfig LoadLiveConfig()
        {
            LiveConfig config;
            std::ifstream input("server.ini");
            if (!input) return config;

            std::string section;
            std::string line;
            while (std::getline(input, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                const auto first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line[first] == ';' || line[first] == '#') continue;
                line.erase(0, first);
                if (line.front() == '[' && line.back() == ']') {
                    section = line.substr(1, line.size() - 2);
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const auto key = line.substr(0, eq);
                const auto value = line.substr(eq + 1);
                try {
                    if (section == "Server" && key == "Port") config.port = static_cast<std::uint16_t>(std::stoul(value));
                    else if (section == "Server" && key == "MaxPlayers") config.maxPlayers = static_cast<std::uint32_t>(std::stoul(value));
                    else if (section == "Network" && key == "TickHz") config.tickHz = static_cast<std::uint32_t>(std::stoul(value));
                } catch (...) {
                    throw std::runtime_error("invalid live server.ini network value");
                }
            }
            if (config.port == 0 || config.maxPlayers == 0 || config.tickHz == 0 || config.tickHz > 120) {
                throw std::runtime_error("invalid live server configuration");
            }
            return config;
        }
    }

    void RunDedicatedServerLoop(RuntimeEntityRegistry& registry, const std::string& loadOrderRevision)
    {
        if (loadOrderRevision.empty()) throw std::runtime_error("live server loop requires load-order revision");

        const auto config = LoadLiveConfig();
        NetworkTransport transport;
        transport.Bind(config.port);
        ServerSessionManager sessions(loadOrderRevision, config.maxPlayers);

        g_stopRequested.store(false, std::memory_order_relaxed);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        const auto tickInterval = std::chrono::microseconds(1000000 / config.tickHz);
        auto nextTick = std::chrono::steady_clock::now();
        auto nextStatus = nextTick + std::chrono::seconds(5);
        std::uint64_t ticks{};
        std::uint64_t replicationPasses{};

        std::cout << "[LIVE] listening=0.0.0.0:" << transport.BoundPort()
                  << " tickHz=" << config.tickHz
                  << " maxPlayers=" << config.maxPlayers
                  << " protocol=" << kWireProtocolVersion
                  << " loadOrder=" << loadOrderRevision << '\n';
        std::cout << "[LIVE] Ctrl+C to stop dedicated server\n";

        while (!g_stopRequested.load(std::memory_order_relaxed)) {
            transport.PollOnce();
            sessions.ProcessControlPackets(transport);
            transport.PumpMaintenance(std::chrono::milliseconds(100), std::chrono::seconds(30));
            sessions.ExpireIdle(std::chrono::seconds(15));

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
                          << " interestUpdates=" << s.interestUpdates
                          << " replicationFrames=" << s.replicationFrames
                          << " replicationPasses=" << replicationPasses
                          << " sent=" << t.datagramsSent
                          << " received=" << t.datagramsReceived
                          << " retransmits=" << t.retransmits << '\n';
                nextStatus = now + std::chrono::seconds(5);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        std::cout << "[LIVE-STOP] ticks=" << ticks
                  << " sessions=" << sessions.SessionCount()
                  << " replicationFrames=" << sessions.Stats().replicationFrames << '\n';
    }
}
