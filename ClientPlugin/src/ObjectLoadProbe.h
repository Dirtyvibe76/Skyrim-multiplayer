#pragma once

#include <cstdint>
#include <vector>

namespace SkyrimMP
{
    struct ObjectLoadEventRecord
    {
        std::uint32_t formId{};
        bool loaded{};
    };

    class ObjectLoadProbe final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        static ObjectLoadProbe* GetSingleton();
        static bool Install();
        static std::vector<ObjectLoadEventRecord> DrainPending();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESObjectLoadedEvent* a_event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>* a_eventSource) override;
    };
}
