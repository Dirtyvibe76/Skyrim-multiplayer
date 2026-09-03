#pragma once

#include "PlayerState.h"

namespace SkyrimMP
{
    class RuntimeProbe
    {
    public:
        static PlayerState ReadLocalPlayer();
        static void LogLocalPlayer();
    };
}
