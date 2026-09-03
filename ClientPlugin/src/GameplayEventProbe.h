#pragma once

#include "pch.h"

namespace SkyrimMP
{
    struct GameplayStatusSnapshot
    {
        bool dead{};
        bool inCombat{};
        bool valid{};
    };

    class GameplayEventProbe final :
        public RE::BSTEventSink<RE::TESCombatEvent>,
        public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        static GameplayEventProbe* GetSingleton();
        static bool Install();
        static void Reset();
        static GameplayStatusSnapshot Snapshot();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESCombatEvent* a_event,
            RE::BSTEventSource<RE::TESCombatEvent>* a_source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESDeathEvent* a_event,
            RE::BSTEventSource<RE::TESDeathEvent>* a_source) override;
    };
}
