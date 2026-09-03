#include "pch.h"

#include "ObjectLoadProbe.h"

namespace SkyrimMP
{
    ObjectLoadProbe* ObjectLoadProbe::GetSingleton()
    {
        static ObjectLoadProbe singleton;
        return std::addressof(singleton);
    }

    bool ObjectLoadProbe::Install()
    {
        auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!sourceHolder) {
            logs::critical("[RE-0.4d] ScriptEventSourceHolder unavailable");
            return false;
        }

        sourceHolder->AddEventSink<RE::TESObjectLoadedEvent>(GetSingleton());
        logs::info("[RE-0.4d] TESObjectLoadedEvent sink installed");
        return true;
    }

    RE::BSEventNotifyControl ObjectLoadProbe::ProcessEvent(
        const RE::TESObjectLoadedEvent* a_event,
        RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Deliberately do not resolve or dereference the form here.
        // This stage validates the engine's native load/unload event stream
        // without touching ProcessLists during actor updates.
        logs::info(
            "[OBJECT {}] form={:08X}",
            a_event->loaded ? "LOAD" : "UNLOAD",
            a_event->formID);

        return RE::BSEventNotifyControl::kContinue;
    }
}
