#include "pch.h"

#include "MultiplayerUiRules.h"

namespace SkyrimMP::MultiplayerUiRules
{
    namespace
    {
        std::atomic_bool g_active{ false };
        bool g_menuRulesInstalled = false;
        bool g_interactionRulesInstalled = false;

        template <class T>
        RE::UI::Create_t*& OriginalCreator()
        {
            static RE::UI::Create_t* creator = nullptr;
            return creator;
        }

        template <class T>
        RE::IMenu* CreateMultiplayerMenu()
        {
            auto* creator = OriginalCreator<T>();
            if (!creator) return nullptr;

            auto* menu = creator();
            if (!menu || !g_active.load(std::memory_order_relaxed)) return menu;

            if (menu->PausesGame()) {
                menu->menuFlags.reset(RE::IMenu::Flag::kPausesGame);
            }
            if (menu->FreezeFrameBackground()) {
                menu->menuFlags.reset(RE::IMenu::Flag::kFreezeFrameBackground);
            }
            if (!menu->RequiresUpdate()) {
                menu->menuFlags.set(RE::IMenu::Flag::kRequiresUpdate);
            }
            return menu;
        }

        template <class T>
        bool WrapMenu(RE::UI& a_ui)
        {
            auto it = a_ui.menuMap.find(T::MENU_NAME);
            if (it == a_ui.menuMap.end() || !it->second.create) {
                logs::warn("[MP UI RULES] menu creator unavailable name={}", T::MENU_NAME);
                return false;
            }

            OriginalCreator<T>() = it->second.create;
            it->second.create = &CreateMultiplayerMenu<T>;
            return true;
        }

        bool LooksLikeRemotePlayerProxy(RE::TESObjectREFR* a_reference)
        {
            auto* actor = a_reference ? a_reference->As<RE::Actor>() : nullptr;
            auto* base = actor ? actor->GetActorBase() : nullptr;
            if (!base) return false;

            // Multiplayer remote-player bases are runtime-created NPC duplicates
            // with this exact visual-shell safety contract.  Requiring all four
            // flags plus the dynamic form namespace keeps ordinary Skyrim actors
            // out of this interaction rule.
            if ((base->GetFormID() & 0xFF000000u) != 0xFF000000u) return false;
            const auto& flags = base->actorData.actorBaseFlags;
            return flags.all(RE::ACTOR_BASE_DATA::Flag::kNoActivation) &&
                   flags.all(RE::ACTOR_BASE_DATA::Flag::kIsGhost) &&
                   flags.all(RE::ACTOR_BASE_DATA::Flag::kInvulnerable) &&
                   flags.all(RE::ACTOR_BASE_DATA::Flag::kDoesntBleed);
        }

        void ShowPlayerBusyNotification()
        {
            using Notify_t = void (*)(const char*, const char*, bool);
            static REL::Relocation<Notify_t> notify{ RELOCATION_ID(52050, 52933) };
            notify("This player is busy.", nullptr, true);
        }

        class InteractionInputSink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static InteractionInputSink* GetSingleton()
            {
                static InteractionInputSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_events,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (!a_events || !IsActive()) return RE::BSEventNotifyControl::kContinue;

                auto* userEvents = RE::UserEvents::GetSingleton();
                if (!userEvents) return RE::BSEventNotifyControl::kContinue;

                for (auto* event = *a_events; event; event = event->next) {
                    auto* button = event->AsButtonEvent();
                    if (!button || !button->IsDown() || button->GetUserEvent() != userEvents->activate) continue;

                    auto* pick = RE::CrosshairPickData::GetSingleton();
                    if (!pick) continue;
                    auto target = pick->GetActiveTarget().get();
                    if (!target || !LooksLikeRemotePlayerProxy(target.get())) continue;

                    ShowPlayerBusyNotification();
                    logs::info("[MP PLAYER INTERACTION BLOCKED] form={:08X}", target->GetFormID());
                    break;
                }

                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    bool InstallMenuRules()
    {
        if (g_menuRulesInstalled) return true;

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;

        std::size_t wrapped = 0;
        wrapped += WrapMenu<RE::TweenMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::InventoryMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::MagicMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::MapMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::StatsMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::FavoritesMenu>(*ui) ? 1u : 0u;
        // JournalMenu owns the System/Save/Load flow. Keep its vanilla pause semantics
        // so Skyrim can serialize a stable world while multiplayer replication is active.
        wrapped += WrapMenu<RE::ContainerMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::BarterMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::GiftMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::CraftingMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::TrainingMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::BookMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::MessageBoxMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::LevelUpMenu>(*ui) ? 1u : 0u;
        wrapped += WrapMenu<RE::LockpickingMenu>(*ui) ? 1u : 0u;

        g_menuRulesInstalled = wrapped != 0;
        logs::info(
            "[MP UI RULES] non-pausing multiplayer menu creators installed wrapped={} exclusions=MainMenu,LoadingMenu,RaceSexMenu,SleepWaitMenu,Console,JournalMenu(SaveLoad)",
            wrapped);
        return g_menuRulesInstalled;
    }

    bool InstallInteractionRules()
    {
        if (g_interactionRulesInstalled) return true;

        auto* input = RE::BSInputDeviceManager::GetSingleton();
        if (!input) return false;

        input->AddEventSink(InteractionInputSink::GetSingleton());
        g_interactionRulesInstalled = true;
        logs::info("[MP UI RULES] remote-player activation feedback installed");
        return true;
    }

    void SetActive(bool a_active) noexcept
    {
        const auto previous = g_active.exchange(a_active, std::memory_order_relaxed);
        if (previous != a_active) {
            logs::info("[MP UI RULES] multiplayer rules active={}", a_active);
        }
    }

    bool IsActive() noexcept
    {
        return g_active.load(std::memory_order_relaxed);
    }
}
