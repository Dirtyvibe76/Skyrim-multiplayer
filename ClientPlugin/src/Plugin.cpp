#include "pch.h"

#include "ClientNetwork.h"
#include "GameplayEventProbe.h"
#include "MainThreadHook.h"
#include "MultiplayerLaunchConfig.h"
#include "ObjectLoadProbe.h"
#include "BuildInfo.h"

namespace
{
    bool g_hookInstalled = false;
    bool g_objectLoadProbeInstalled = false;
    bool g_networkStarted = false;
    bool g_gameplayEventProbeInstalled = false;
    bool g_createBootstrapRequested = false;
    bool g_menuEventSinkInstalled = false;

    void StartMultiplayerNetwork();

    bool RunSystemCommand(std::string_view command)
    {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        auto* form = factory ? factory->Create() : nullptr;
        auto* script = form ? form->As<RE::Script>() : nullptr;
        if (!script) return false;
        script->SetCommand(command);
        script->CompileAndRun(nullptr);
        script->ClearCommand();
        return true;
    }

    class MainMenuOpenSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MainMenuOpenSink* GetSingleton()
        {
            static MainMenuOpenSink singleton;
            return std::addressof(singleton);
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event || !a_event->opening || a_event->menuName != RE::MainMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const auto& launch = SkyrimMP::GetMultiplayerLaunchConfig();
            if (!launch.createCharacter || launch.characterId == 0 || g_createBootstrapRequested) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                logs::error("[MP CHARACTER BOOTSTRAP] SKSE task interface unavailable at main menu");
                return RE::BSEventNotifyControl::kContinue;
            }

            g_createBootstrapRequested = true;
            const auto characterId = launch.characterId;
            logs::info(
                "[MP CHARACTER BOOTSTRAP] main menu ready; queueing Riverwood creation character={:016X}",
                characterId);
            tasks->AddTask([characterId]() {
                if (RunSystemCommand("coc Riverwood")) {
                    logs::info(
                        "[MP CHARACTER BOOTSTRAP] requested new multiplayer-only player in Riverwood character={:016X}",
                        characterId);
                    StartMultiplayerNetwork();
                    logs::info("[MP CHARACTER BOOTSTRAP] Riverwood multiplayer client worker started");
                } else {
                    g_createBootstrapRequested = false;
                    logs::error("[MP CHARACTER BOOTSTRAP] failed to dispatch coc Riverwood");
                }
            });

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void StartMultiplayerNetwork()
    {
        SkyrimMP::MainThreadHook::ResetActorCache();
        SkyrimMP::GameplayEventProbe::Reset();
        if (g_networkStarted) SkyrimMP::ClientNetwork::Stop();
        SkyrimMP::ClientNetwork::Start();
        g_networkStarted = true;
    }

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

            if (!g_menuEventSinkInstalled) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->AddEventSink<RE::MenuOpenCloseEvent>(MainMenuOpenSink::GetSingleton());
                    g_menuEventSinkInstalled = true;
                    logs::info("[MP CHARACTER BOOTSTRAP] main-menu readiness sink installed");
                } else {
                    logs::error("[MP CHARACTER BOOTSTRAP] UI unavailable; main-menu readiness sink not installed");
                }
            }

            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logs::info("[ALPHA {}] save loaded", SkyrimMP::BuildInfo::kVersion);
            StartMultiplayerNetwork();
            logs::info("[ALPHA {}] loaded-save multiplayer client worker started", SkyrimMP::BuildInfo::kVersion);
            break;

        case SKSE::MessagingInterface::kNewGame:
            logs::info("[ALPHA {}] new game", SkyrimMP::BuildInfo::kVersion);
            if (SkyrimMP::GetMultiplayerLaunchConfig().createCharacter) {
                StartMultiplayerNetwork();
                logs::info("[ALPHA {}] new multiplayer-only character worker started", SkyrimMP::BuildInfo::kVersion);
            } else {
                SkyrimMP::MainThreadHook::ResetActorCache();
                SkyrimMP::GameplayEventProbe::Reset();
                if (g_networkStarted) {
                    SkyrimMP::ClientNetwork::Stop();
                    g_networkStarted = false;
                }
                logs::info("[ALPHA {}] ordinary new game remains single-player until explicitly launched for character creation", SkyrimMP::BuildInfo::kVersion);
            }
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
