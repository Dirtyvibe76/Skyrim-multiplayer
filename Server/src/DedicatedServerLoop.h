#pragma once

#include "RuntimeEntityRegistry.h"

#include <string>

namespace SkyrimMP::Server
{
    void RunDedicatedServerLoop(
        RuntimeEntityRegistry& a_registry,
        const std::string& a_loadOrderRevision);
}
