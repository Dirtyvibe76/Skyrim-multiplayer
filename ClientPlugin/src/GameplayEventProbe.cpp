#include "pch.h"

#include "GameplayEventProbe.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace SkyrimMP
{
    namespace
    {
        std::atomic_bool g_dead{ false };
        std::atomic_bool g_inCombat{ false };
        std::atomic_bool g_valid{ false };
        std::mutex g_equipmentMutex;
        std::vector<std::uint32_t> g_equippedFormIds;
        constexpr std::size_t kMaxEquipmentItems = 32;

        bool IsPlayer(const RE::TESObjectREFR* reference)
        {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            return reference && player && reference->GetFormID() == player->GetFormID();
        }
    }

    GameplayEventProbe* GameplayEventProbe::GetSingleton()
    {
        static GameplayEventProbe singleton;
        return &singleton;
    }

    bool GameplayEventProbe::Install()
    {
        auto* source = RE::ScriptEventSourceHolder::GetSingleton();
        if (!source) return false;
        source->AddEventSink<RE::TESCombatEvent>(GetSingleton());
        source->AddEventSink<RE::TESDeathEvent>(GetSingleton());
        source->AddEventSink<RE::TESEquipEvent>(GetSingleton());
        logs::info("[GAMEPLAY EVENTS] player combat, death, and equipment sinks installed");
        return true;
    }

    void GameplayEventProbe::Reset()
    {
        g_dead.store(false, std::memory_order_relaxed);
        g_inCombat.store(false, std::memory_order_relaxed);
        g_valid.store(true, std::memory_order_release);
        std::scoped_lock lock(g_equipmentMutex);
        g_equippedFormIds.clear();
    }

    GameplayStatusSnapshot GameplayEventProbe::Snapshot()
    {
        GameplayStatusSnapshot result;
        result.valid = g_valid.load(std::memory_order_acquire);
        result.dead = g_dead.load(std::memory_order_relaxed);
        result.inCombat = g_inCombat.load(std::memory_order_relaxed);
        {
            std::scoped_lock lock(g_equipmentMutex);
            result.equippedFormIds = g_equippedFormIds;
        }
        return result;
    }

    RE::BSEventNotifyControl GameplayEventProbe::ProcessEvent(
        const RE::TESCombatEvent* event,
        RE::BSTEventSource<RE::TESCombatEvent>*)
    {
        if (event && IsPlayer(event->actor.get())) {
            g_inCombat.store(event->newState != RE::ACTOR_COMBAT_STATE::kNone, std::memory_order_relaxed);
            g_valid.store(true, std::memory_order_release);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl GameplayEventProbe::ProcessEvent(
        const RE::TESDeathEvent* event,
        RE::BSTEventSource<RE::TESDeathEvent>*)
    {
        if (event && IsPlayer(event->actorDying.get())) {
            g_dead.store(event->dead, std::memory_order_relaxed);
            if (event->dead) g_inCombat.store(false, std::memory_order_relaxed);
            g_valid.store(true, std::memory_order_release);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl GameplayEventProbe::ProcessEvent(
        const RE::TESEquipEvent* event,
        RE::BSTEventSource<RE::TESEquipEvent>*)
    {
        if (!event || !IsPlayer(event->actor.get()) || event->baseObject == 0) {
            return RE::BSEventNotifyControl::kContinue;
        }
        std::scoped_lock lock(g_equipmentMutex);
        const auto it = std::find(g_equippedFormIds.begin(), g_equippedFormIds.end(), event->baseObject);
        if (event->equipped) {
            if (it == g_equippedFormIds.end() && g_equippedFormIds.size() < kMaxEquipmentItems) {
                g_equippedFormIds.push_back(event->baseObject);
                std::sort(g_equippedFormIds.begin(), g_equippedFormIds.end());
            }
        } else if (it != g_equippedFormIds.end()) {
            g_equippedFormIds.erase(it);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
}
