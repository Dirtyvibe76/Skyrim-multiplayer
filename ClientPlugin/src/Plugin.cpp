#include "pch.h"

#include "ClientNetwork.h"
#include "GameplayEventProbe.h"
#include "MainThreadHook.h"
#include "ObjectLoadProbe.h"
#include "BuildInfo.h"

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
            logs::info("[ALPHA {}] Skyrim data loaded", SkyrimMP::BuildInfo::kVersion);

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
            logs::info("[ALPHA {}] save loaded", SkyrimMP::BuildInfo::kVersion);
            SkyrimMP::MainThreadHook::ResetActorCache();
            SkyrimMP::GameplayEventProbe::Reset();
            if (g_networkStarted) SkyrimMP::ClientNetwork::Stop();
            SkyrimMP::ClientNetwork::Start();
            g_networkStarted = true;
            logs::info("[ALPHA {}] loaded-save multiplayer client worker started", SkyrimMP::BuildInfo::kVersion);
            break;

        case SKSE::MessagingInterface::kNewGame:
            logs::info("[ALPHA {}] new game", SkyrimMP::BuildInfo::kVersion);
            SkyrimMP::MainThreadHook::ResetActorCache();
            SkyrimMP::GameplayEventProbe::Reset();
            if (g_networkStarted) {
                SkyrimMP::ClientNetwork::Stop();
                g_networkStarted = false;
            }
            logs::info("[ALPHA {}] multiplayer waits for a post-Helgen saved game", SkyrimMP::BuildInfo::kVersion);
            break;

        default:
            break;
        }
    }
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    logs::info(
        "SkyrimMP client version={} channel={} wireProtocol={} replicationProtocol={}",
        SkyrimMP::BuildInfo::kVersion,
        SkyrimMP::BuildInfo::kChannel,
        SkyrimMP::BuildInfo::kWireProtocol,
        SkyrimMP::BuildInfo::kReplicationProtocol);

    const auto runtime = REL::Module::get().version();

    logs::info(
        "Runtime version: {}.{}.{}.{}",
        runtime.major(),
        runtime.minor(),
        runtime.patch(),
        runtime.build());

    auto* messaging = SKSE::GetMessagingInterface();

    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logs::critical("[ALPHA {}] failed to register SKSE messaging listener", SkyrimMP::BuildInfo::kVersion);
        return false;
    }

    return true;
}
