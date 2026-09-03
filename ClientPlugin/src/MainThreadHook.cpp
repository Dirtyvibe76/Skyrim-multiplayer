#include "pch.h"

#include "MainThreadHook.h"
#include "RuntimeProbe.h"
#include "ObjectLoadProbe.h"

#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        std::unordered_map<std::uint32_t, std::uint32_t> g_knownActors;
    }

    void MainThreadHook::Install()
    {
        REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
        originalUpdate = playerVTable.write_vfunc(0xAD, Update);

        logs::info("[RE-0.4f] PlayerCharacter::Update hook installed; actor classification enabled");
    }

    void MainThreadHook::ResetActorCache()
    {
        g_knownActors.clear();
        logs::info("[RE-0.4f] actor discovery cache reset");
    }

    void MainThreadHook::Update(RE::Actor* a_actor, float a_delta)
    {
        originalUpdate(a_actor, a_delta);

        static auto lastPlayerSample = std::chrono::steady_clock::time_point{};
        static bool firstUpdateLogged = false;

        if (!firstUpdateLogged) {
            logs::info("[RE-0.4f] PlayerCharacter::Update hook executing");
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
    }
}
