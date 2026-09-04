#pragma once

#include "pch.h"

namespace SkyrimMP
{
    struct GameplayStatusSnapshot
    {
        bool dead{};
        bool inCombat{};
        bool equipmentValid{};
        bool valid{};
        std::vector<std::uint32_t> equippedFormIds;
    };

    class GameplayEventProbe final :
        public RE::BSTEventSink<RE::TESCombatEvent>,
        public RE::BSTEventSink<RE::TESDeathEvent>,
        public RE::BSTEventSink<RE::TESEquipEvent>
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
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESEquipEvent* a_event,
            RE::BSTEventSource<RE::TESEquipEvent>* a_source) override;
    };
}
