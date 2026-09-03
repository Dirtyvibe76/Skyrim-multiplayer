#include "pch.h"

#include "MainThreadHook.h"
#include "RuntimeProbe.h"
#include "ObjectLoadProbe.h"

namespace SkyrimMP
{
    void MainThreadHook::Install()
    {
        REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
        originalUpdate = playerVTable.write_vfunc(0xAD, Update);

        logs::info("[RE-0.4e] PlayerCharacter::Update hook installed; actor traversal disabled");
    }

    void MainThreadHook::Update(RE::Actor* a_actor, float a_delta)
    {
        originalUpdate(a_actor, a_delta);

        static auto lastPlayerSample = std::chrono::steady_clock::time_point{};
        static bool firstUpdateLogged = false;

        if (!firstUpdateLogged) {
            logs::info("[RE-0.4e] PlayerCharacter::Update hook executing");
            firstUpdateLogged = true;
        }

        const auto now = std::chrono::steady_clock::now();

        if (lastPlayerSample.time_since_epoch().count() == 0 ||
            now - lastPlayerSample >= 100ms) {

            RuntimeProbe::LogLocalPlayer();
            lastPlayerSample = now;
        }

        // Drain copied load/unload POD records here. This validates the
        // cross-thread handoff without resolving or dereferencing forms yet.
        const auto pending = ObjectLoadProbe::DrainPending();

        for (const auto& event : pending) {
            logs::info(
                "[OBJECT HANDOFF {}] form={:08X}",
                event.loaded ? "LOAD" : "UNLOAD",
                event.formId);
        }
    }
}
