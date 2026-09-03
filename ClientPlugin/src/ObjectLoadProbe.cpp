#include "pch.h"

#include "ObjectLoadProbe.h"

#include <mutex>
#include <utility>

namespace SkyrimMP
{
    namespace
    {
        std::mutex g_pendingMutex;
        std::vector<ObjectLoadEventRecord> g_pendingEvents;
    }

    ObjectLoadProbe* ObjectLoadProbe::GetSingleton()
    {
        static ObjectLoadProbe singleton;
        return std::addressof(singleton);
    }

    bool ObjectLoadProbe::Install()
    {
        auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!sourceHolder) {
            logs::critical("[RE-0.4e] ScriptEventSourceHolder unavailable");
            return false;
        }

        sourceHolder->AddEventSink<RE::TESObjectLoadedEvent>(GetSingleton());
        logs::info("[RE-0.4e] TESObjectLoadedEvent sink installed");
        return true;
    }

    std::vector<ObjectLoadEventRecord> ObjectLoadProbe::DrainPending()
    {
        std::scoped_lock lock(g_pendingMutex);

        std::vector<ObjectLoadEventRecord> pending;
        pending.swap(g_pendingEvents);
        return pending;
    }

    RE::BSEventNotifyControl ObjectLoadProbe::ProcessEvent(
        const RE::TESObjectLoadedEvent* a_event,
        RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Event callbacks have been observed on multiple OS thread IDs.
        // Copy only POD data here. Do not resolve or dereference Skyrim forms.
        {
            std::scoped_lock lock(g_pendingMutex);
            g_pendingEvents.push_back(ObjectLoadEventRecord{
                a_event->formID,
                a_event->loaded
            });
        }

        return RE::BSEventNotifyControl::kContinue;
    }
}
