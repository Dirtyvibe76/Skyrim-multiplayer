#include "pch.h"

#include "RuntimeProbe.h"

namespace
{
    std::atomic_bool g_probeStarted{ false };

    void StartProbeScheduler()
    {
        bool expected = false;

        if (!g_probeStarted.compare_exchange_strong(expected, true)) {
            return;
        }

        logs::info("[RE-0.4a] player probe scheduler started; actor probe disabled pending main-thread hook");

        std::thread([]()
        {
            while (true) {
                std::this_thread::sleep_for(100ms);

                auto* taskInterface = SKSE::GetTaskInterface();
                if (!taskInterface) {
                    continue;
                }

                taskInterface->AddTask([]()
                {
                    SkyrimMP::RuntimeProbe::LogLocalPlayer();
                });
            }
        }).detach();
    }

    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }

        switch (a_message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            logs::info("[RE-0.4a] Skyrim data loaded");
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logs::info("[RE-0.4a] save loaded");
            StartProbeScheduler();
            break;

        case SKSE::MessagingInterface::kNewGame:
            logs::info("[RE-0.4a] new game");
            StartProbeScheduler();
            break;

        default:
            break;
        }
    }
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    logs::info("Skyrim Multiplayer RE-0.4a loaded");

    const auto runtime = REL::Module::get().version();

    logs::info(
        "Runtime version: {}.{}.{}.{}",
        runtime.major(),
        runtime.minor(),
        runtime.patch(),
        runtime.build());

    auto* messaging = SKSE::GetMessagingInterface();

    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logs::critical("[RE-0.4a] failed to register SKSE messaging listener");
        return false;
    }

    return true;
}
