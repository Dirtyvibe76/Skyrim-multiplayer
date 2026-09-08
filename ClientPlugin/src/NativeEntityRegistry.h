#pragma once

#include "WorldEntityId.h"

#include <cstddef>
#include <cstdint>

namespace SkyrimMP
{
    enum class NativeEntityKind : std::uint8_t
    {
        StaticReference,
        RemotePlayer,
        DynamicReference
    };

    class NativeEntityRegistry
    {
    public:
        static bool BindStatic(WorldEntityId a_id, std::uint32_t a_runtimeFormId);
        static bool BindRuntime(WorldEntityId a_id, RE::TESObjectREFR& a_reference, NativeEntityKind a_kind);
        static RE::TESObjectREFR* Resolve(WorldEntityId a_id);
        static WorldEntityId FindByRuntimeFormId(std::uint32_t a_runtimeFormId);
        static void Unbind(WorldEntityId a_id);
        static void ResetStatic();
        static void Reset();
        static std::size_t Size();
    };
}
