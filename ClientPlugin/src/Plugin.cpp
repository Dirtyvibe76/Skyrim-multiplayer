#include "pch.h"

#include "MainThreadHook.h"
#include "ObjectLoadProbe.h"

namespace
{
    bool g_hookInstalled = false;
    bool g_objectLoadProbeInstalled = false;

    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }

        switch (a_message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            logs::info("[RE-0.7a] Skyrim data loaded");

            if (!g_hookInstalled) {
                SkyrimMP::MainThreadHook::Install();
                g_hookInstalled = true;
            }

            if (!g_objectLoadProbeInstalled) {
                g_objectLoadProbeInstalled = SkyrimMP::ObjectLoadProbe::Install();
            }
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logs::info("[RE-0.7a] save loaded");
            SkyrimMP::MainThreadHook::ResetActorCache();
            break;

        case SKSE::MessagingInterface::kNewGame:
            logs::info("[RE-0.7a] new game");
            SkyrimMP::MainThreadHook::ResetActorCache();
            break;

        default:
            break;
        }
    }
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    logs::info("Skyrim Multiplayer RE-0.7a loaded");

    const auto runtime = REL::Module::get().version();

    logs::info(
        "Runtime version: {}.{}.{}.{}",
        runtime.major(),
        runtime.minor(),
        runtime.patch(),
        runtime.build());

    auto* messaging = SKSE::GetMessagingInterface();

    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logs::critical("[RE-0.7a] failed to register SKSE messaging listener");
        return false;
    }

    return true;
}
