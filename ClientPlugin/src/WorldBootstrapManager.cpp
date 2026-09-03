#include "pch.h"

#include "WorldBootstrapManager.h"

#include <atomic>
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

        bool ContextMatches(const ServerWorldBootstrap& bootstrap, RE::PlayerCharacter* player)
        {
            if (!player) return false;
            auto* cell = player->GetParentCell();
            if (!cell || cell->GetFormID() != bootstrap.cellFormId) return false;

            std::uint32_t worldFormId = 0;
            if (auto* world = cell->GetRuntimeData().worldSpace) worldFormId = world->GetFormID();
            return worldFormId == bootstrap.worldspaceFormId;
        }
    }

    void WorldBootstrapManager::Enqueue(const ServerWorldBootstrap& bootstrap)
    {
        if (bootstrap.playerEntityId == 0 || bootstrap.anchorRuntimeFormId == 0 || bootstrap.cellFormId == 0) {
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

        auto* anchorForm = RE::TESForm::LookupByID(bootstrap.anchorRuntimeFormId);
        auto* anchor = anchorForm ? anchorForm->As<RE::TESObjectREFR>() : nullptr;
        if (!anchor) {
            logs::warn("[WORLD BOOTSTRAP WAIT] anchor={:08X} reason=reference unavailable", bootstrap.anchorRuntimeFormId);
            return 0;
        }

        // The anchor is a server-selected vanilla placed reference. MoveTo performs the
        // cell/world transition using Skyrim's own relocation path; no background thread
        // touches runtime objects.
        player->MoveTo(anchor);
        player->SetPosition(RE::NiPoint3{
            bootstrap.position.x,
            bootstrap.position.y,
            bootstrap.position.z
        });
        player->data.angle.x = bootstrap.rotation.x;
        player->data.angle.y = bootstrap.rotation.y;
        player->data.angle.z = bootstrap.rotation.z;
        player->Update3DPosition(true);

        if (!ContextMatches(bootstrap, player)) {
            logs::warn(
                "[WORLD BOOTSTRAP WAIT] playerEntity={:016X} anchor={:08X} reason=context transition pending",
                bootstrap.playerEntityId,
                bootstrap.anchorRuntimeFormId);
            return 0;
        }

        const auto observed = player->GetPosition();
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
        g_applied.store(false, std::memory_order_release);
        logs::info("[WORLD BOOTSTRAP] reset");
    }

    bool WorldBootstrapManager::HasApplied()
    {
        return g_applied.load(std::memory_order_acquire);
    }
}
