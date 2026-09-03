#pragma once

#include "PlayerState.h"

namespace SkyrimMP
{
    class ClientNetwork
    {
    public:
        static void Start();
        static void Stop();
        static void SubmitLocalPlayer(const PlayerState& a_player);
        static bool IsAuthenticated();
    };
}
