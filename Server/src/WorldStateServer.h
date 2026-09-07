#pragma once

#include "RuntimeEntityRegistry.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace SkyrimMP::Server
{
    struct WorldStateServerStats
    {
        std::uint64_t clientsRegistered{};
        std::uint64_t observationsReceived{};
        std::uint64_t observationsApplied{};
        std::uint64_t observationsRejected{};
        std::uint64_t snapshotsSent{};
        std::uint64_t persistedStates{};
    };

    class WorldStateServer
    {
    public:
        WorldStateServer();
        ~WorldStateServer();
        WorldStateServer(const WorldStateServer&) = delete;
        WorldStateServer& operator=(const WorldStateServer&) = delete;

        void Start(
            RuntimeEntityRegistry& a_registry,
            std::uint16_t a_port,
            std::string a_loadOrderRevision,
            std::filesystem::path a_statePath);
        void Poll();
        void Stop();

        std::uint16_t BoundPort() const noexcept;
        std::size_t ClientCount() const noexcept;
        const WorldStateServerStats& Stats() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
