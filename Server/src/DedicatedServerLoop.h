#pragma once

#include "RuntimeEntityRegistry.h"

#include <cstdint>
#include <string>

namespace SkyrimMP::Server
{
    void RunDedicatedServerLoop(
        RuntimeEntityRegistry& a_registry,
        const std::string& a_loadOrderRevision,
        std::uint16_t a_port,
        std::uint32_t a_maxPlayers,
        std::uint32_t a_tickHz);
}
