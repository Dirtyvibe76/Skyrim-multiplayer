#include "pch.h"

#include "GameplayEventProbe.h"

namespace SkyrimMP
{
    namespace
    {
        std::atomic_bool g_dead{ false };
        std::atomic_bool g_inCombat{ false };
        std::atomic_bool g_valid{ false };

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
        logs::info("[GAMEPLAY EVENTS] player combat and death sinks installed");
        return true;
    }

    void GameplayEventProbe::Reset()
    {
        g_dead.store(false, std::memory_order_relaxed);
        g_inCombat.store(false, std::memory_order_relaxed);
        g_valid.store(true, std::memory_order_release);
    }

    GameplayStatusSnapshot GameplayEventProbe::Snapshot()
    {
        GameplayStatusSnapshot result;
        result.valid = g_valid.load(std::memory_order_acquire);
        result.dead = g_dead.load(std::memory_order_relaxed);
        result.inCombat = g_inCombat.load(std::memory_order_relaxed);
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
}
