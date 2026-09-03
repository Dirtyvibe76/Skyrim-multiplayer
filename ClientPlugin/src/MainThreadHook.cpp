#include "pch.h"

#include "MainThreadHook.h"
#include "RuntimeProbe.h"
#include "ObjectLoadProbe.h"
#include "RemoteActorAdapter.h"
#include "ActorState.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimMP
{
    namespace
    {
        constexpr float kExteriorRelevanceRadius = 12000.0f;
        constexpr float kExteriorRelevanceRadiusSquared =
            kExteriorRelevanceRadius * kExteriorRelevanceRadius;
        constexpr float kMoveThreshold = 32.0f;
        constexpr float kMoveThresholdSquared = kMoveThreshold * kMoveThreshold;
        constexpr float kRotationThreshold = 0.05f;
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;

        std::unordered_map<std::uint32_t, std::uint32_t> g_knownActors;
        std::unordered_set<std::uint32_t> g_relevantActors;
        std::unordered_map<std::uint32_t, ActorState> g_lastRelevantState;

        ActorState ReadActorState(RE::Actor* a_actor, std::uint32_t a_baseFormId)
        {
            ActorState state{};
            if (!a_actor) {
                return state;
            }

            state.runtimeFormId = a_actor->GetFormID();
            state.baseFormId = a_baseFormId;

            const auto position = a_actor->GetPosition();
            state.position = { position.x, position.y, position.z };

            const auto angle = a_actor->GetAngle();
            state.rotation = { angle.x, angle.y, angle.z };

            if (auto* cell = a_actor->GetParentCell()) {
                state.cellFormId = cell->GetFormID();
                if (auto* worldspace = cell->GetRuntimeData().worldSpace) {
                    state.worldspaceFormId = worldspace->GetFormID();
                }
            }

            return state;
        }

        bool IsRelevantToPlayer(const ActorState& a_actor, const PlayerState& a_player)
        {
            if (a_actor.runtimeFormId == 0 ||
                a_actor.runtimeFormId == a_player.formId ||
                a_actor.cellFormId == 0 ||
                a_player.cellFormId == 0) {
                return false;
            }

            if (a_player.worldspaceFormId == 0) {
                return a_actor.worldspaceFormId == 0 &&
                       a_actor.cellFormId == a_player.cellFormId;
            }

            if (a_actor.worldspaceFormId != a_player.worldspaceFormId) {
                return false;
            }

            const float dx = a_actor.position.x - a_player.position.x;
            const float dy = a_actor.position.y - a_player.position.y;
            const float dz = a_actor.position.z - a_player.position.z;
            return dx * dx + dy * dy + dz * dz <= kExteriorRelevanceRadiusSquared;
        }

        bool PositionChanged(const ActorState& a_current, const ActorState& a_previous)
        {
            const float dx = a_current.position.x - a_previous.position.x;
            const float dy = a_current.position.y - a_previous.position.y;
            const float dz = a_current.position.z - a_previous.position.z;
            return dx * dx + dy * dy + dz * dz >= kMoveThresholdSquared;
        }

        float WrappedAngleDelta(float a_current, float a_previous)
        {
            float delta = std::fmod(std::fabs(a_current - a_previous), kTwoPi);
            if (delta > kPi) {
                delta = kTwoPi - delta;
            }
            return delta;
        }

        bool RotationChanged(const ActorState& a_current, const ActorState& a_previous)
        {
            return WrappedAngleDelta(a_current.rotation.x, a_previous.rotation.x) >= kRotationThreshold ||
                   WrappedAngleDelta(a_current.rotation.y, a_previous.rotation.y) >= kRotationThreshold ||
                   WrappedAngleDelta(a_current.rotation.z, a_previous.rotation.z) >= kRotationThreshold;
        }

        void LogSpawn(const ActorState& a_state)
        {
            logs::info(
                "[ACTOR SPAWN] form={:08X} base={:08X} cell={:08X} world={:08X} "
                "pos=({:.2f},{:.2f},{:.2f}) rot=({:.3f},{:.3f},{:.3f})",
                a_state.runtimeFormId,
                a_state.baseFormId,
                a_state.cellFormId,
                a_state.worldspaceFormId,
                a_state.position.x,
                a_state.position.y,
                a_state.position.z,
                a_state.rotation.x,
                a_state.rotation.y,
                a_state.rotation.z);
        }

        void SampleRelevantActors()
        {
            const auto playerState = RuntimeProbe::ReadLocalPlayer();
            if (playerState.formId == 0 || playerState.cellFormId == 0) {
                return;
            }

            std::unordered_set<std::uint32_t> currentRelevant;

            for (const auto& [runtimeFormId, baseFormId] : g_knownActors) {
                if (runtimeFormId == playerState.formId) {
                    continue;
                }

                auto* form = RE::TESForm::LookupByID(runtimeFormId);
                if (!form) {
                    continue;
                }

                auto* actor = form->As<RE::Actor>();
                if (!actor) {
                    continue;
                }

                const auto state = ReadActorState(actor, baseFormId);
                if (!IsRelevantToPlayer(state, playerState)) {
                    continue;
                }

                currentRelevant.insert(runtimeFormId);

                const auto previousIt = g_lastRelevantState.find(runtimeFormId);
                if (previousIt == g_lastRelevantState.end()) {
                    logs::info(
                        "[ACTOR RELEVANCE ENTER] form={:08X} base={:08X} cell={:08X} world={:08X}",
                        state.runtimeFormId,
                        state.baseFormId,
                        state.cellFormId,
                        state.worldspaceFormId);
                    LogSpawn(state);
                    g_lastRelevantState.insert_or_assign(runtimeFormId, state);
                    continue;
                }

                const auto& previous = previousIt->second;

                if (state.cellFormId != previous.cellFormId ||
                    state.worldspaceFormId != previous.worldspaceFormId) {
                    logs::info(
                        "[ACTOR TRANSITION] form={:08X} cell={:08X}->{:08X} world={:08X}->{:08X}",
                        state.runtimeFormId,
                        previous.cellFormId,
                        state.cellFormId,
                        previous.worldspaceFormId,
                        state.worldspaceFormId);
                }

                if (PositionChanged(state, previous)) {
                    logs::info(
                        "[ACTOR MOVE] form={:08X} pos=({:.2f},{:.2f},{:.2f})",
                        state.runtimeFormId,
                        state.position.x,
                        state.position.y,
                        state.position.z);
                }

                if (RotationChanged(state, previous)) {
                    logs::info(
                        "[ACTOR ROTATE] form={:08X} rot=({:.3f},{:.3f},{:.3f})",
                        state.runtimeFormId,
                        state.rotation.x,
                        state.rotation.y,
                        state.rotation.z);
                }

                g_lastRelevantState.insert_or_assign(runtimeFormId, state);
            }

            for (const auto runtimeFormId : g_relevantActors) {
                if (!currentRelevant.contains(runtimeFormId)) {
                    const auto it = g_knownActors.find(runtimeFormId);
                    const auto baseFormId = it != g_knownActors.end() ? it->second : 0;
                    logs::info(
                        "[ACTOR RELEVANCE EXIT] form={:08X} base={:08X}",
                        runtimeFormId,
                        baseFormId);
                    logs::info(
                        "[ACTOR DESPAWN] form={:08X} base={:08X} reason=relevance",
                        runtimeFormId,
                        baseFormId);
                    g_lastRelevantState.erase(runtimeFormId);
                }
            }

            const bool setChanged = currentRelevant != g_relevantActors;
            g_relevantActors = std::move(currentRelevant);

            if (setChanged) {
                logs::info(
                    "[ACTOR RELEVANCE SET] relevant={} known={} mode={} cell={:08X} world={:08X}",
                    g_relevantActors.size(),
                    g_knownActors.size(),
                    playerState.worldspaceFormId == 0 ? "interior" : "exterior",
                    playerState.cellFormId,
                    playerState.worldspaceFormId);
            }

        }
    }

    void MainThreadHook::Install()
    {
        REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
        originalUpdate = playerVTable.write_vfunc(0xAD, Update);

        logs::info("[RE-0.7a] PlayerCharacter::Update hook installed; inbound RemoteActorAdapter queue armed");
    }

    void MainThreadHook::ResetActorCache()
    {
        g_knownActors.clear();
        g_relevantActors.clear();
        g_lastRelevantState.clear();
        RemoteActorAdapter::Reset();
        logs::info("[CLIENT] actor caches and remote adapter reset");
    }

    void MainThreadHook::Update(RE::Actor* a_actor, float a_delta)
    {
        originalUpdate(a_actor, a_delta);

        static auto lastPlayerSample = std::chrono::steady_clock::time_point{};
        static auto lastActorSample = std::chrono::steady_clock::time_point{};
        static bool firstUpdateLogged = false;

        if (!firstUpdateLogged) {
            logs::info("[RE-0.7a] PlayerCharacter::Update hook executing");
            firstUpdateLogged = true;
        }

        const auto now = std::chrono::steady_clock::now();

        if (lastPlayerSample.time_since_epoch().count() == 0 ||
            now - lastPlayerSample >= 100ms) {
            RuntimeProbe::LogLocalPlayer();
            lastPlayerSample = now;
        }

        const auto pending = ObjectLoadProbe::DrainPending();

        for (const auto& event : pending) {
            if (!event.loaded) {
                const bool wasRelevant = g_relevantActors.erase(event.formId) > 0;
                g_lastRelevantState.erase(event.formId);

                const auto it = g_knownActors.find(event.formId);
                if (it != g_knownActors.end()) {
                    if (wasRelevant) {
                        logs::info(
                            "[ACTOR DESPAWN] form={:08X} base={:08X} reason=unload",
                            it->first,
                            it->second);
                    }

                    logs::info(
                        "[ACTOR UNLOAD] form={:08X} base={:08X}",
                        it->first,
                        it->second);
                    g_knownActors.erase(it);
                }
                continue;
            }

            auto* form = RE::TESForm::LookupByID(event.formId);
            if (!form) {
                continue;
            }

            auto* actor = form->As<RE::Actor>();
            if (!actor) {
                continue;
            }

            std::uint32_t baseFormId = 0;
            if (auto* actorBase = actor->GetActorBase()) {
                baseFormId = actorBase->GetFormID();
            }

            const auto [it, inserted] = g_knownActors.insert_or_assign(event.formId, baseFormId);
            if (inserted) {
                logs::info(
                    "[ACTOR DISCOVER] form={:08X} base={:08X}",
                    it->first,
                    it->second);
            }
        }

        if (lastActorSample.time_since_epoch().count() == 0 ||
            now - lastActorSample >= 500ms) {
            SampleRelevantActors();
            lastActorSample = now;
        }

        RemoteActorAdapter::ApplyPending(32);
    }
}
