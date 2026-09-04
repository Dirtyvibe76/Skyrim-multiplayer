#include "pch.h"

#include "WorldBootstrapManager.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>

namespace SkyrimMP
{
    namespace
    {
        std::mutex g_mutex;
        std::optional<ServerWorldBootstrap> g_pending;
        std::atomic_bool g_applied{ false };
        std::uint64_t g_appliedPlayerEntityId{};
        bool g_characterCreatorRequested{};
        bool g_characterCreatorObservedOpen{};
        std::uint32_t g_characterCreatorAttempts{};
        std::chrono::steady_clock::time_point g_lastCharacterCreatorRequest{};

        bool ContextMatches(const ServerWorldBootstrap& bootstrap, RE::PlayerCharacter* player)
        {
            if (!player) return false;
            auto* cell = player->GetParentCell();
            if (!cell) return false;

            std::uint32_t worldFormId = 0;
            if (auto* world = cell->GetRuntimeData().worldSpace) worldFormId = world->GetFormID();
            if (bootstrap.worldspaceFormId != 0) {
                // Persistent exterior references can be owned by Tamriel's
                // persistent CELL while their coordinates stream a neighboring
                // exterior CELL. Worldspace is the authoritative context there.
                return worldFormId == bootstrap.worldspaceFormId;
            }
            return cell->GetFormID() == bootstrap.cellFormId;
        }

        bool RunShowRaceMenu(RE::PlayerCharacter& player)
        {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
            if (!factory) {
                logs::warn("[MP CHARACTER CREATE WAIT] reason=Script form factory unavailable");
                return false;
            }

            auto* form = factory->Create();
            auto* script = form ? form->As<RE::Script>() : nullptr;
            if (!script) {
                logs::warn("[MP CHARACTER CREATE WAIT] reason=Script form creation failed");
                return false;
            }

            script->SetCommand("showracemenu");
            script->CompileAndRun(&player);
            script->ClearCommand();
            return true;
        }

        bool CompleteFirstLoginCharacterCreation(RE::PlayerCharacter& player)
        {
            auto* ui = RE::UI::GetSingleton();
            const auto now = std::chrono::steady_clock::now();

            if (!g_characterCreatorRequested) {
                if (!RunShowRaceMenu(player)) return false;
                g_characterCreatorRequested = true;
                g_characterCreatorAttempts = 1;
                g_lastCharacterCreatorRequest = now;
                logs::info("[MP CHARACTER CREATE] requested native RaceSex Menu through showracemenu before first multiplayer save");
                return false;
            }

            if (!g_characterCreatorObservedOpen) {
                if (ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
                    g_characterCreatorObservedOpen = true;
                    logs::info("[MP CHARACTER CREATE] RaceSex Menu open; waiting for player confirmation");
                    return false;
                }

                // showracemenu is dispatched through Skyrim's script command
                // machinery and can be delayed while the Riverwood transfer is
                // still settling. Retry a bounded number of times instead of
                // leaving multiplayer bootstrap permanently stuck.
                if (g_characterCreatorAttempts < 3 && now - g_lastCharacterCreatorRequest >= std::chrono::seconds(2)) {
                    if (RunShowRaceMenu(player)) {
                        ++g_characterCreatorAttempts;
                        g_lastCharacterCreatorRequest = now;
                        logs::warn("[MP CHARACTER CREATE RETRY] attempt={} RaceSex Menu not observed open yet", g_characterCreatorAttempts);
                    }
                }
                return false;
            }

            if (ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) return false;

            logs::info("[MP CHARACTER CREATE] RaceSex Menu closed; multiplayer appearance accepted");
            return true;
        }
    }

    void WorldBootstrapManager::Enqueue(const ServerWorldBootstrap& bootstrap)
    {
        if (bootstrap.playerEntityId == 0 || bootstrap.cellFormId == 0) {
            logs::warn("[WORLD BOOTSTRAP DROP] reason=invalid identity anchor={:08X} cell={:08X}",
                bootstrap.anchorRuntimeFormId,
                bootstrap.cellFormId);
            return;
        }

        std::scoped_lock lock(g_mutex);
        if (g_applied.load(std::memory_order_relaxed) && g_appliedPlayerEntityId == bootstrap.playerEntityId) return;
        g_pending = bootstrap;
        logs::info(
            "[WORLD BOOTSTRAP QUEUED] playerEntity={:016X} anchor={:08X} cell={:08X} world={:08X} pos=({:.2f},{:.2f},{:.2f})",
            bootstrap.playerEntityId,
            bootstrap.anchorRuntimeFormId,
            bootstrap.cellFormId,
            bootstrap.worldspaceFormId,
            bootstrap.position.x,
            bootstrap.position.y,
            bootstrap.position.z);
    }

    std::size_t WorldBootstrapManager::ApplyPending()
    {
        ServerWorldBootstrap bootstrap{};
        {
            std::scoped_lock lock(g_mutex);
            if (!g_pending) return 0;
            bootstrap = *g_pending;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return 0;

        if (bootstrap.anchorRuntimeFormId != 0) {
            auto* anchorForm = RE::TESForm::LookupByID(bootstrap.anchorRuntimeFormId);
            auto* anchor = anchorForm ? anchorForm->As<RE::TESObjectREFR>() : nullptr;
            if (!anchor) {
                logs::warn("[WORLD BOOTSTRAP WAIT] anchor={:08X} reason=reference unavailable", bootstrap.anchorRuntimeFormId);
                return 0;
            }

            // Move only until the first-login context has actually been reached.
            // Once RaceSex Menu is open, repeatedly calling MoveTo would disturb
            // the character creator camera and its temporary player state.
            if (!ContextMatches(bootstrap, player) && !g_characterCreatorRequested) {
                player->MoveTo(anchor);
            }
        } else if (!ContextMatches(bootstrap, player)) {
            logs::warn(
                "[WORLD BOOTSTRAP WAIT] playerEntity={:016X} reason=local context not ready",
                bootstrap.playerEntityId);
            return 0;
        }

        if (!g_characterCreatorRequested) {
            player->SetPosition(RE::NiPoint3{
                bootstrap.position.x,
                bootstrap.position.y,
                bootstrap.position.z
            }, true);
            player->data.angle.x = bootstrap.rotation.x;
            player->data.angle.y = bootstrap.rotation.y;
            player->data.angle.z = bootstrap.rotation.z;
            player->Update3DPosition(true);
        }

        if (!ContextMatches(bootstrap, player)) {
            logs::warn(
                "[WORLD BOOTSTRAP WAIT] playerEntity={:016X} anchor={:08X} reason=context transition pending",
                bootstrap.playerEntityId,
                bootstrap.anchorRuntimeFormId);
            return 0;
        }

        // A non-zero anchor is the server's one-time onboarding assignment.
        // Do not create the SkyrimMP save until the player has explicitly made
        // their multiplayer character. This prevents an arbitrary single-player
        // appearance from becoming the multiplayer identity by accident.
        if (bootstrap.anchorRuntimeFormId != 0 && !CompleteFirstLoginCharacterCreation(*player)) {
            return 0;
        }

        const auto observed = player->GetPosition();

        if (bootstrap.anchorRuntimeFormId != 0) {
            if (auto* saves = RE::BGSSaveLoadManager::GetSingleton()) {
                char branchName[64]{};
                std::snprintf(branchName, sizeof(branchName), "SkyrimMP_%08X", saves->currentCharacterID);
                saves->Save(branchName);
                logs::info("[WORLD BOOTSTRAP SAVE] branch={} multiplayerCharacterCreated=true sourcePreserved=true", branchName);
            } else {
                logs::error("[WORLD BOOTSTRAP SAVE] failed: save manager unavailable");
                return 0;
            }
        }

        {
            std::scoped_lock lock(g_mutex);
            g_pending.reset();
            g_appliedPlayerEntityId = bootstrap.playerEntityId;
            g_applied.store(true, std::memory_order_release);
        }

        logs::info(
            "[WORLD BOOTSTRAP APPLIED] playerEntity={:016X} anchor={:08X} cell={:08X} world={:08X} obsPos=({:.2f},{:.2f},{:.2f}) authority=server",
            bootstrap.playerEntityId,
            bootstrap.anchorRuntimeFormId,
            bootstrap.cellFormId,
            bootstrap.worldspaceFormId,
            observed.x,
            observed.y,
            observed.z);
        return 1;
    }

    void WorldBootstrapManager::Reset()
    {
        std::scoped_lock lock(g_mutex);
        g_pending.reset();
        g_appliedPlayerEntityId = 0;
        g_characterCreatorRequested = false;
        g_characterCreatorObservedOpen = false;
        g_characterCreatorAttempts = 0;
        g_lastCharacterCreatorRequest = {};
        g_applied.store(false, std::memory_order_release);
        logs::info("[WORLD BOOTSTRAP] reset");
    }

    bool WorldBootstrapManager::HasApplied()
    {
        return g_applied.load(std::memory_order_acquire);
    }
}
