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

        bool CompleteFirstLoginCharacterCreation()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!g_characterCreatorRequested) {
                auto* queue = RE::UIMessageQueue::GetSingleton();
                if (!queue) {
                    logs::warn("[MP CHARACTER CREATE WAIT] reason=UI message queue unavailable");
                    return false;
                }
                queue->AddMessage(RE::RaceSexMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                g_characterCreatorRequested = true;
                logs::info("[MP CHARACTER CREATE] requested native RaceSex Menu before first multiplayer save");
                return false;
            }

            if (!g_characterCreatorObservedOpen) {
                if (ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
                    g_characterCreatorObservedOpen = true;
                    logs::info("[MP CHARACTER CREATE] RaceSex Menu open; waiting for player confirmation");
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
        if (bootstrap.anchorRuntimeFormId != 0 && !CompleteFirstLoginCharacterCreation()) {
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
        g_applied.store(false, std::memory_order_release);
        logs::info("[WORLD BOOTSTRAP] reset");
    }

    bool WorldBootstrapManager::HasApplied()
    {
        return g_applied.load(std::memory_order_acquire);
    }
}
