#pragma once

#include "ActorState.h"
#include "PlayerState.h"

namespace SkyrimMP
{
    class ClientNetwork
    {
    public:
        static void Start();
        static void Stop();
        static void SubmitLocalPlayer(const PlayerState& a_player);
        static void SubmitLocalActor(const ActorState& a_actor);
        static bool IsAuthenticated();
    };
}
