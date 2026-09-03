#pragma once

namespace SkyrimMP
{
    class ObjectLoadProbe final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        static ObjectLoadProbe* GetSingleton();
        static bool Install();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESObjectLoadedEvent* a_event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>* a_eventSource) override;
    };
}
