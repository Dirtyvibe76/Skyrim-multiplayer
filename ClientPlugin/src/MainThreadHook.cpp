#include "pch.h"

#include "MainThreadHook.h"
#include "RuntimeProbe.h"
#include "ObjectLoadProbe.h"
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
        constexpr float kWriteProbeRotationOffset = 0.05f;
        constexpr auto kWriteProbeDelay = 5s;

        std::unordered_map<std::uint32_t, std::uint32_t> g_knownActors;
        std::unordered_set<std::uint32_t> g_relevantActors;
        std::unordered_map<std::uint32_t, ActorState> g_lastRelevantState;

        bool g_writeProbeComplete = false;
        std::uint32_t g_writeProbeCandidate = 0;
        std::chrono::steady_clock::time_point g_writeProbeCandidateSince{};

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
            const float distanceSquared = dx * dx + dy * dy + dz * dz;

            return distanceSquared <= kExteriorRelevanceRadiusSquared;
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

        float NormalizeAngle(float a_angle)
        {
            float normalized = std::fmod(a_angle, kTwoPi);
            if (normalized < 0.0f) {
                normalized += kTwoPi;
            }
            return normalized;
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

        void ResetWriteProbeCandidate()
        {
            g_writeProbeCandidate = 0;
            g_writeProbeCandidateSince = {};
        }

        void ConsiderWriteProbeCandidate(std::uint32_t a_runtimeFormId)
        {
            if (g_writeProbeComplete || a_runtimeFormId == 0) {
                return;
            }

            if (g_writeProbeCandidate != a_runtimeFormId) {
                g_writeProbeCandidate = a_runtimeFormId;
                g_writeProbeCandidateSince = std::chrono::steady_clock::now();
                logs::info("[REMOTE ROTATION PROBE] candidate form={:08X}; waiting 5 seconds", a_runtimeFormId);
            }
        }

        void RunControlledRotationWriteProbe()
        {
            if (g_writeProbeComplete ||
                g_writeProbeCandidate == 0 ||
                g_writeProbeCandidateSince.time_since_epoch().count() == 0 ||
                std::chrono::steady_clock::now() - g_writeProbeCandidateSince < kWriteProbeDelay) {
                return;
            }

            if (!g_relevantActors.contains(g_writeProbeCandidate)) {
                ResetWriteProbeCandidate();
                return;
            }

            const auto knownIt = g_knownActors.find(g_writeProbeCandidate);
            if (knownIt == g_knownActors.end()) {
                ResetWriteProbeCandidate();
                return;
            }

            auto* form = RE::TESForm::LookupByID(g_writeProbeCandidate);
            if (!form) {
                ResetWriteProbeCandidate();
                return;
            }

            auto* actor = form->As<RE::Actor>();
            if (!actor || !actor->Get3D()) {
                ResetWriteProbeCandidate();
                return;
            }

            const auto before = ReadActorState(actor, knownIt->second);
            if (before.runtimeFormId == 0 || before.cellFormId == 0) {
                ResetWriteProbeCandidate();
                return;
            }

            const float requestedZ = NormalizeAngle(before.rotation.z + kWriteProbeRotationOffset);

            logs::info(
                "[REMOTE ROTATION BEGIN] form={:08X} before=({:.3f},{:.3f},{:.3f}) requestedZ={:.3f}",
                before.runtimeFormId,
                before.rotation.x,
                before.rotation.y,
                before.rotation.z,
                requestedZ);

            // CommonLib exposes reference rotation through TESObjectREFR::data.angle.
            // Update3DPosition(true) applies that transform to the live 3D/Havok state.
            // Change yaw only, then immediately restore the original angle.
            actor->data.angle.z = requestedZ;
            actor->Update3DPosition(true);

            const auto appliedAngle = actor->GetAngle();
            logs::info(
                "[REMOTE ROTATION APPLIED] form={:08X} observed=({:.3f},{:.3f},{:.3f})",
                before.runtimeFormId,
                appliedAngle.x,
                appliedAngle.y,
                appliedAngle.z);

            actor->data.angle.x = before.rotation.x;
            actor->data.angle.y = before.rotation.y;
            actor->data.angle.z = before.rotation.z;
            actor->Update3DPosition(true);

            const auto restoredAngle = actor->GetAngle();
            logs::info(
                "[REMOTE ROTATION RESTORED] form={:08X} observed=({:.3f},{:.3f},{:.3f})",
                before.runtimeFormId,
                restoredAngle.x,
                restoredAngle.y,
                restoredAngle.z);

            g_writeProbeComplete = true;
            ResetWriteProbeCandidate();
        }

        void SampleRelevantActors()
        {
            const auto playerState = RuntimeProbe::ReadLocalPlayer();
            if (playerState.formId == 0 || playerState.cellFormId == 0) {
                return;
            }

            std::unordered_set<std::uint32_t> currentRelevant;
            std::uint32_t firstRelevant = 0;

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
                if (firstRelevant == 0 && actor->Get3D()) {
                    firstRelevant = runtimeFormId;
                }

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

            if (!g_writeProbeComplete) {
                if (g_writeProbeCandidate == 0 || !g_relevantActors.contains(g_writeProbeCandidate)) {
                    if (firstRelevant != 0) {
                        ConsiderWriteProbeCandidate(firstRelevant);
                    } else {
                        ResetWriteProbeCandidate();
                    }
                }
            }
        }
    }

    void MainThreadHook::Install()
    {
        REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
        originalUpdate = playerVTable.write_vfunc(0xAD, Update);

        logs::info("[RE-0.5b] PlayerCharacter::Update hook installed; controlled inbound rotation write probe armed");
    }

    void MainThreadHook::ResetActorCache()
    {
        g_knownActors.clear();
        g_relevantActors.clear();
        g_lastRelevantState.clear();
        g_writeProbeComplete = false;
        ResetWriteProbeCandidate();
        logs::info("[RE-0.5b] actor caches reset; controlled rotation write probe re-armed");
    }

    void MainThreadHook::Update(RE::Actor* a_actor, float a_delta)
    {
        originalUpdate(a_actor, a_delta);

        static auto lastPlayerSample = std::chrono::steady_clock::time_point{};
        static auto lastActorSample = std::chrono::steady_clock::time_point{};
        static bool firstUpdateLogged = false;

        if (!firstUpdateLogged) {
            logs::info("[RE-0.5b] PlayerCharacter::Update hook executing");
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

                if (event.formId == g_writeProbeCandidate) {
                    ResetWriteProbeCandidate();
                }

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
            RunControlledRotationWriteProbe();
            lastActorSample = now;
        }
    }
}
