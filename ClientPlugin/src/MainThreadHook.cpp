#include "pch.h"

#include "MainThreadHook.h"
#include "RuntimeProbe.h"
#include "ObjectLoadProbe.h"
#include "ActorState.h"

#include <cmath>
#include <limits>
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

        constexpr auto kStreamArmDelay = 5s;
        constexpr auto kStreamCadence = 100ms;
        constexpr auto kStreamDuration = 10s;
        constexpr float kStreamRadius = 120.0f;
        constexpr float kStreamPitchAmplitude = 0.15f;
        constexpr float kStreamYawSweep = kTwoPi;

        std::unordered_map<std::uint32_t, std::uint32_t> g_knownActors;
        std::unordered_set<std::uint32_t> g_relevantActors;
        std::unordered_map<std::uint32_t, ActorState> g_lastRelevantState;

        bool g_streamComplete = false;
        bool g_streamActive = false;
        std::uint32_t g_streamCandidate = 0;
        std::chrono::steady_clock::time_point g_streamCandidateSince{};
        std::chrono::steady_clock::time_point g_streamStarted{};
        std::chrono::steady_clock::time_point g_streamLastApplied{};
        ActorState g_streamOriginal{};
        std::uint32_t g_streamTick = 0;

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

        float DistanceSquaredToPlayer(const ActorState& a_actor, const PlayerState& a_player)
        {
            const float dx = a_actor.position.x - a_player.position.x;
            const float dy = a_actor.position.y - a_player.position.y;
            const float dz = a_actor.position.z - a_player.position.z;
            return dx * dx + dy * dy + dz * dz;
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

        float ClampPitch(float a_pitch)
        {
            const float limit = kPi * 0.5f;
            if (a_pitch < -limit) {
                return -limit;
            }
            if (a_pitch > limit) {
                return limit;
            }
            return a_pitch;
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

        void ResetStreamCandidate()
        {
            if (!g_streamActive) {
                g_streamCandidate = 0;
                g_streamCandidateSince = {};
            }
        }

        void ConsiderStreamCandidate(std::uint32_t a_runtimeFormId, float a_distanceSquared)
        {
            if (g_streamComplete || g_streamActive || a_runtimeFormId == 0) {
                return;
            }

            if (g_streamCandidate != a_runtimeFormId) {
                g_streamCandidate = a_runtimeFormId;
                g_streamCandidateSince = std::chrono::steady_clock::now();
                logs::info(
                    "[REMOTE STREAM PROBE] nearest candidate form={:08X} distance={:.1f}; waiting 5 seconds before 10 Hz / 10 second transform stream",
                    a_runtimeFormId,
                    std::sqrt(a_distanceSquared));
            }
        }

        void ClearStreamState(bool a_complete)
        {
            g_streamActive = false;
            g_streamComplete = a_complete;
            g_streamCandidate = 0;
            g_streamCandidateSince = {};
            g_streamStarted = {};
            g_streamLastApplied = {};
            g_streamOriginal = {};
            g_streamTick = 0;
        }

        void AbortActiveStream(const char* a_reason)
        {
            if (!g_streamActive) {
                ResetStreamCandidate();
                return;
            }

            logs::warn(
                "[REMOTE STREAM ABORT] form={:08X} reason={}; re-arming nearest-actor selection",
                g_streamCandidate,
                a_reason);
            ClearStreamState(false);
        }

        void RestoreStreamActor(RE::Actor* a_actor)
        {
            if (!a_actor || g_streamOriginal.runtimeFormId == 0) {
                return;
            }

            const RE::NiPoint3 originalPosition{
                g_streamOriginal.position.x,
                g_streamOriginal.position.y,
                g_streamOriginal.position.z
            };
            a_actor->SetPosition(originalPosition, true);
            a_actor->data.angle.x = g_streamOriginal.rotation.x;
            a_actor->data.angle.y = g_streamOriginal.rotation.y;
            a_actor->data.angle.z = g_streamOriginal.rotation.z;
            a_actor->Update3DPosition(true);

            const auto restoredPosition = a_actor->GetPosition();
            const auto restoredAngle = a_actor->GetAngle();
            logs::info(
                "[REMOTE STREAM RESTORED] form={:08X} pos=({:.2f},{:.2f},{:.2f}) rot=({:.3f},{:.3f},{:.3f}) ticks={}",
                g_streamOriginal.runtimeFormId,
                restoredPosition.x,
                restoredPosition.y,
                restoredPosition.z,
                restoredAngle.x,
                restoredAngle.y,
                restoredAngle.z,
                g_streamTick);
        }

        void RunSyntheticRemoteTransformStream()
        {
            if (g_streamComplete || g_streamCandidate == 0) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();

            if (!g_streamActive) {
                if (g_streamCandidateSince.time_since_epoch().count() == 0 ||
                    now - g_streamCandidateSince < kStreamArmDelay) {
                    return;
                }

                if (!g_relevantActors.contains(g_streamCandidate)) {
                    ResetStreamCandidate();
                    return;
                }

                const auto knownIt = g_knownActors.find(g_streamCandidate);
                if (knownIt == g_knownActors.end()) {
                    ResetStreamCandidate();
                    return;
                }

                auto* form = RE::TESForm::LookupByID(g_streamCandidate);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor || !actor->Get3D()) {
                    ResetStreamCandidate();
                    return;
                }

                g_streamOriginal = ReadActorState(actor, knownIt->second);
                if (g_streamOriginal.runtimeFormId == 0 || g_streamOriginal.cellFormId == 0) {
                    ResetStreamCandidate();
                    return;
                }

                g_streamActive = true;
                g_streamStarted = now;
                g_streamLastApplied = {};
                g_streamTick = 0;

                logs::info(
                    "[REMOTE STREAM BEGIN] form={:08X} originPos=({:.2f},{:.2f},{:.2f}) originRot=({:.3f},{:.3f},{:.3f}) cadence=100ms duration=10s radius=120",
                    g_streamOriginal.runtimeFormId,
                    g_streamOriginal.position.x,
                    g_streamOriginal.position.y,
                    g_streamOriginal.position.z,
                    g_streamOriginal.rotation.x,
                    g_streamOriginal.rotation.y,
                    g_streamOriginal.rotation.z);
            }

            if (now - g_streamStarted >= kStreamDuration) {
                auto* form = RE::TESForm::LookupByID(g_streamCandidate);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (actor && actor->Get3D()) {
                    RestoreStreamActor(actor);
                    logs::info("[REMOTE STREAM COMPLETE] form={:08X}", g_streamCandidate);
                } else {
                    logs::warn(
                        "[REMOTE STREAM COMPLETE] form={:08X} actor unavailable at restore",
                        g_streamCandidate);
                }
                ClearStreamState(true);
                return;
            }

            if (g_streamLastApplied.time_since_epoch().count() != 0 &&
                now - g_streamLastApplied < kStreamCadence) {
                return;
            }

            if (!g_relevantActors.contains(g_streamCandidate)) {
                AbortActiveStream("actor left relevance set");
                return;
            }

            auto* form = RE::TESForm::LookupByID(g_streamCandidate);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor || !actor->Get3D()) {
                AbortActiveStream("actor unavailable");
                return;
            }

            const float elapsedSeconds =
                std::chrono::duration<float>(now - g_streamStarted).count();
            const float phase = std::clamp(elapsedSeconds / 10.0f, 0.0f, 1.0f);
            const float orbit = phase * kTwoPi;

            const RE::NiPoint3 requestedPosition{
                g_streamOriginal.position.x + std::cos(orbit) * kStreamRadius,
                g_streamOriginal.position.y + std::sin(orbit) * kStreamRadius,
                g_streamOriginal.position.z
            };
            const float requestedPitch = ClampPitch(
                g_streamOriginal.rotation.x + std::sin(orbit * 2.0f) * kStreamPitchAmplitude);
            const float requestedYaw = NormalizeAngle(
                g_streamOriginal.rotation.z + phase * kStreamYawSweep);

            actor->SetPosition(requestedPosition, true);
            actor->data.angle.x = requestedPitch;
            actor->data.angle.y = g_streamOriginal.rotation.y;
            actor->data.angle.z = requestedYaw;
            actor->Update3DPosition(true);

            const auto observedPosition = actor->GetPosition();
            const auto observedAngle = actor->GetAngle();
            ++g_streamTick;
            g_streamLastApplied = now;

            logs::info(
                "[REMOTE STREAM TICK] form={:08X} tick={} phase={:.3f} reqPos=({:.2f},{:.2f},{:.2f}) obsPos=({:.2f},{:.2f},{:.2f}) reqRot=({:.3f},{:.3f}) obsRot=({:.3f},{:.3f})",
                g_streamCandidate,
                g_streamTick,
                phase,
                requestedPosition.x,
                requestedPosition.y,
                requestedPosition.z,
                observedPosition.x,
                observedPosition.y,
                observedPosition.z,
                requestedPitch,
                requestedYaw,
                observedAngle.x,
                observedAngle.z);
        }

        void SampleRelevantActors()
        {
            const auto playerState = RuntimeProbe::ReadLocalPlayer();
            if (playerState.formId == 0 || playerState.cellFormId == 0) {
                return;
            }

            std::unordered_set<std::uint32_t> currentRelevant;
            std::uint32_t nearestRelevant = 0;
            float nearestDistanceSquared = (std::numeric_limits<float>::max)();

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

                if (actor->Get3D()) {
                    const float distanceSquared = DistanceSquaredToPlayer(state, playerState);
                    if (distanceSquared < nearestDistanceSquared) {
                        nearestDistanceSquared = distanceSquared;
                        nearestRelevant = runtimeFormId;
                    }
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

            if (!g_streamComplete && !g_streamActive) {
                if (nearestRelevant != 0) {
                    ConsiderStreamCandidate(nearestRelevant, nearestDistanceSquared);
                } else {
                    ResetStreamCandidate();
                }
            }
        }
    }

    void MainThreadHook::Install()
    {
        REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
        originalUpdate = playerVTable.write_vfunc(0xAD, Update);

        logs::info("[RE-0.6b] PlayerCharacter::Update hook installed; nearest-actor sustained synthetic remote transform stream armed");
    }

    void MainThreadHook::ResetActorCache()
    {
        g_knownActors.clear();
        g_relevantActors.clear();
        g_lastRelevantState.clear();
        g_streamComplete = false;
        g_streamActive = false;
        g_streamCandidate = 0;
        g_streamCandidateSince = {};
        g_streamStarted = {};
        g_streamLastApplied = {};
        g_streamOriginal = {};
        g_streamTick = 0;
        logs::info("[RE-0.6b] actor caches reset; nearest-actor sustained transform stream re-armed");
    }

    void MainThreadHook::Update(RE::Actor* a_actor, float a_delta)
    {
        originalUpdate(a_actor, a_delta);

        static auto lastPlayerSample = std::chrono::steady_clock::time_point{};
        static auto lastActorSample = std::chrono::steady_clock::time_point{};
        static bool firstUpdateLogged = false;

        if (!firstUpdateLogged) {
            logs::info("[RE-0.6b] PlayerCharacter::Update hook executing");
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

                if (event.formId == g_streamCandidate) {
                    if (g_streamActive) {
                        AbortActiveStream("actor unloaded");
                    } else {
                        ResetStreamCandidate();
                    }
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
            lastActorSample = now;
        }

        RunSyntheticRemoteTransformStream();
    }
}
