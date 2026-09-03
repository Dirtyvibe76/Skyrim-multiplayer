#pragma once

#include "RemoteTransform.h"

#include <cstddef>

namespace SkyrimMP
{
    class RemoteActorAdapter
    {
    public:
        static void Enqueue(const RemoteTransform& a_transform);
        static std::size_t ApplyPending(std::size_t a_budget = 32);
        static void Reset();
    };
}
