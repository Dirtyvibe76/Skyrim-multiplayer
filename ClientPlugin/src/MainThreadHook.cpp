#include "pch.h"

#include "MainThreadHook.h"
#include "RuntimeProbe.h"
#include "ObjectLoadProbe.h"
#include "ActorState.h"

#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        std::unordered_map<std::uint32_t, std::uint32_t> g_knownActors;

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

        void SampleKnownActors()
        {
            for (const auto& [runtimeFormId, baseFormId] : g_knownActors) {
                auto* form = RE::TESForm::LookupByID(runtimeFormId);
                if (!form) {
                    continue;
                }

                auto* actor = form->As<RE::Actor>();
                if (!actor) {
                    continue;
                }

                const auto state = ReadActorState(actor, baseFormId);

                logs::info(
                    "[ACTOR SNAPSHOT] form={:08X} base={:08X} cell={:08X} world={:08X} "
                    "pos=({:.2f},{:.2f},{:.2f}) rot=({:.3f},{:.3f},{:.3f})",
                    state.runtimeFormId,
                    state.baseFormId,
                    state.cellFormId,
                    state.worldspaceFormId,
                    state.position.x,
                    state.position.y,
                    state.position.z,
                    state.rotation.x,
                    state.rotation.y,
                    state.rotation.z);
            }
        }
    }

    void MainThreadHook::Install()
    {
        REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
        originalUpdate = playerVTable.write_vfunc(0xAD, Update);

        logs::info("[RE-0.4g] PlayerCharacter::Update hook installed; minimal actor snapshots enabled");
    }

    void MainThreadHook::ResetActorCache()
    {
        g_knownActors.clear();
        logs::info("[RE-0.4g] actor discovery cache reset");
    }

    void MainThreadHook::Update(RE::Actor* a_actor, float a_delta)
    {
        originalUpdate(a_actor, a_delta);

        static auto lastPlayerSample = std::chrono::steady_clock::time_point{};
        static auto lastActorSample = std::chrono::steady_clock::time_point{};
        static bool firstUpdateLogged = false;

        if (!firstUpdateLogged) {
            logs::info("[RE-0.4g] PlayerCharacter::Update hook executing");
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
                const auto it = g_knownActors.find(event.formId);
                if (it != g_knownActors.end()) {
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

            SampleKnownActors();
            lastActorSample = now;
        }
    }
}
