#include "pch.h"

#include "ClientNetwork.h"
#include "GameplayEventProbe.h"
#include "MainThreadHook.h"
#include "ObjectLoadProbe.h"

namespace
{
    bool g_hookInstalled = false;
    bool g_objectLoadProbeInstalled = false;
    bool g_networkStarted = false;
    bool g_gameplayEventProbeInstalled = false;

    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }

        switch (a_message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            logs::info("[RE-0.8] Skyrim data loaded");

            if (!g_hookInstalled) {
                SkyrimMP::MainThreadHook::Install();
                g_hookInstalled = true;
            }

            if (!g_objectLoadProbeInstalled) {
                g_objectLoadProbeInstalled = SkyrimMP::ObjectLoadProbe::Install();
            }
            if (!g_gameplayEventProbeInstalled) {
                g_gameplayEventProbeInstalled = SkyrimMP::GameplayEventProbe::Install();
            }

            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logs::info("[RE-0.8] save loaded");
            SkyrimMP::MainThreadHook::ResetActorCache();
            SkyrimMP::GameplayEventProbe::Reset();
            if (g_networkStarted) SkyrimMP::ClientNetwork::Stop();
            SkyrimMP::ClientNetwork::Start();
            g_networkStarted = true;
            logs::info("[RE-0.8] loaded-save multiplayer client worker started");
            break;

        case SKSE::MessagingInterface::kNewGame:
            logs::info("[RE-0.8] new game");
            SkyrimMP::MainThreadHook::ResetActorCache();
            SkyrimMP::GameplayEventProbe::Reset();
            if (g_networkStarted) {
                SkyrimMP::ClientNetwork::Stop();
                g_networkStarted = false;
            }
            logs::info("[RE-0.8] multiplayer waits for a post-Helgen saved game");
            break;

        default:
            break;
        }
    }
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    logs::info("Skyrim Multiplayer RE-0.8 loaded");

    const auto runtime = REL::Module::get().version();

    logs::info(
        "Runtime version: {}.{}.{}.{}",
        runtime.major(),
        runtime.minor(),
        runtime.patch(),
        runtime.build());

    auto* messaging = SKSE::GetMessagingInterface();

    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logs::critical("[RE-0.8] failed to register SKSE messaging listener");
        return false;
    }

    return true;
}
