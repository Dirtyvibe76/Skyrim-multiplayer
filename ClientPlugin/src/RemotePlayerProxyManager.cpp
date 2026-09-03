#include "pch.h"

#include "RemotePlayerProxyManager.h"
#include "RemoteActorAdapter.h"
#include "RemoteTransform.h"

#include <deque>
#include <mutex>
#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        constexpr std::size_t kMaxNativeProxies = 1;

        enum class ProxyCommandKind : std::uint8_t
        {
            Upsert,
            Despawn
        };

        struct ProxyCommand
        {
            ProxyCommandKind kind{ ProxyCommandKind::Upsert };
            RemotePlayerProxyUpdate update;
            std::uint64_t networkEntityId{};
        };

        struct NativeProxy
        {
            RE::ObjectRefHandle handle;
            std::uint64_t lastRevision{};
            std::uint32_t cellFormId{};
            std::uint32_t worldspaceFormId{};
        };

        std::mutex g_mutex;
        std::deque<std::uint64_t> g_order;
        std::unordered_map<std::uint64_t, ProxyCommand> g_pending;
        std::unordered_map<std::uint64_t, NativeProxy> g_proxies;

        bool IsDynamicPlayerEntity(std::uint64_t id)
        {
            return (id & (1ull << 63)) != 0;
        }

        void QueueLocked(ProxyCommand command)
        {
            if (!IsDynamicPlayerEntity(command.networkEntityId)) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=not dynamic namespace", command.networkEntityId);
                return;
            }

            const auto existing = g_pending.find(command.networkEntityId);
            if (existing != g_pending.end()) {
                if (command.kind == ProxyCommandKind::Despawn ||
                    existing->second.kind == ProxyCommandKind::Despawn ||
                    command.update.revision >= existing->second.update.revision) {
                    existing->second = command;
                }
                return;
            }

            g_order.push_back(command.networkEntityId);
            g_pending.emplace(command.networkEntityId, std::move(command));
        }

        RE::Actor* ResolveActor(const NativeProxy& proxy)
        {
            auto reference = proxy.handle.get();
            return reference ? reference->As<RE::Actor>() : nullptr;
        }

        void DestroyProxy(std::uint64_t networkEntityId, NativeProxy& proxy, const char* reason)
        {
            if (auto* actor = ResolveActor(proxy)) {
                actor->EnableAI(false);
                actor->SetCollision(false);
                actor->Disable();
            }
            logs::info("[REMOTE PLAYER PROXY DESPAWN] networkId={:016X} reason={}", networkEntityId, reason);
        }

        bool LocalContextMatches(const RemotePlayerProxyUpdate& update, RE::PlayerCharacter* player)
        {
            if (!player || update.cellFormId == 0) return false;
            auto* cell = player->GetParentCell();
            if (!cell || cell->GetFormID() != update.cellFormId) return false;

            std::uint32_t localWorld = 0;
            if (auto* world = cell->GetRuntimeData().worldSpace) localWorld = world->GetFormID();
            return localWorld == update.worldspaceFormId;
        }

        bool SpawnProxy(std::uint64_t networkEntityId, const RemotePlayerProxyUpdate& update)
        {
            if (g_proxies.size() >= kMaxNativeProxies) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=controlled proxy limit limit={}", networkEntityId, kMaxNativeProxies);
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!LocalContextMatches(update, player)) return false;

            auto* base = player->GetActorBase();
            if (!base) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=local player base unavailable", networkEntityId);
                return false;
            }

            auto placed = player->PlaceObjectAtMe(base, false);
            auto* actor = placed ? placed->As<RE::Actor>() : nullptr;
            if (!actor) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=PlaceObjectAtMe failed", networkEntityId);
                return false;
            }

            actor->SetTemporary();
            actor->SetActivationBlocked(true);
            actor->SetCollision(false);
            actor->EnableAI(false);

            NativeProxy proxy;
            proxy.handle = actor->GetHandle();
            proxy.lastRevision = update.revision;
            proxy.cellFormId = update.cellFormId;
            proxy.worldspaceFormId = update.worldspaceFormId;
            g_proxies.emplace(networkEntityId, proxy);

            logs::info(
                "[REMOTE PLAYER PROXY SPAWN] networkId={:016X} form={:08X} cell={:08X} world={:08X} revision={} mode=single-controlled",
                networkEntityId,
                actor->GetFormID(),
                update.cellFormId,
                update.worldspaceFormId,
                update.revision);

            RemoteActorAdapter::Enqueue(RemoteTransform{
                actor->GetFormID(),
                static_cast<std::uint32_t>(update.revision),
                update.position,
                update.rotation
            });
            return true;
        }

        void ApplyUpsert(const RemotePlayerProxyUpdate& update)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!LocalContextMatches(update, player)) {
                const auto proxyIt = g_proxies.find(update.networkEntityId);
                if (proxyIt != g_proxies.end()) {
                    DestroyProxy(update.networkEntityId, proxyIt->second, "local context changed");
                    g_proxies.erase(proxyIt);
                }
                return;
            }

            auto proxyIt = g_proxies.find(update.networkEntityId);
            if (proxyIt == g_proxies.end()) {
                SpawnProxy(update.networkEntityId, update);
                return;
            }

            auto& proxy = proxyIt->second;
            if (update.revision < proxy.lastRevision) return;

            if (proxy.cellFormId != update.cellFormId || proxy.worldspaceFormId != update.worldspaceFormId) {
                DestroyProxy(update.networkEntityId, proxy, "remote context changed");
                g_proxies.erase(proxyIt);
                SpawnProxy(update.networkEntityId, update);
                return;
            }

            auto* actor = ResolveActor(proxy);
            if (!actor || !actor->Get3D()) {
                DestroyProxy(update.networkEntityId, proxy, "actor unavailable");
                g_proxies.erase(proxyIt);
                SpawnProxy(update.networkEntityId, update);
                return;
            }

            proxy.lastRevision = update.revision;
            RemoteActorAdapter::Enqueue(RemoteTransform{
                actor->GetFormID(),
                static_cast<std::uint32_t>(update.revision),
                update.position,
                update.rotation
            });
        }
    }

    void RemotePlayerProxyManager::EnqueueUpsert(const RemotePlayerProxyUpdate& update)
    {
        if (update.networkEntityId == 0 || update.cellFormId == 0) return;
        std::scoped_lock lock(g_mutex);
        ProxyCommand command;
        command.kind = ProxyCommandKind::Upsert;
        command.networkEntityId = update.networkEntityId;
        command.update = update;
        QueueLocked(std::move(command));
    }

    void RemotePlayerProxyManager::EnqueueDespawn(std::uint64_t networkEntityId)
    {
        if (networkEntityId == 0) return;
        std::scoped_lock lock(g_mutex);
        ProxyCommand command;
        command.kind = ProxyCommandKind::Despawn;
        command.networkEntityId = networkEntityId;
        command.update.networkEntityId = networkEntityId;
        QueueLocked(std::move(command));
    }

    std::size_t RemotePlayerProxyManager::ApplyPending(std::size_t budget)
    {
        std::deque<ProxyCommand> batch;
        {
            std::scoped_lock lock(g_mutex);
            while (!g_order.empty() && batch.size() < budget) {
                const auto id = g_order.front();
                g_order.pop_front();
                const auto it = g_pending.find(id);
                if (it == g_pending.end()) continue;
                batch.push_back(it->second);
                g_pending.erase(it);
            }
        }

        std::size_t applied = 0;
        for (const auto& command : batch) {
            if (command.kind == ProxyCommandKind::Despawn) {
                const auto it = g_proxies.find(command.networkEntityId);
                if (it != g_proxies.end()) {
                    DestroyProxy(command.networkEntityId, it->second, "server despawn");
                    g_proxies.erase(it);
                    ++applied;
                }
                continue;
            }

            ApplyUpsert(command.update);
            ++applied;
        }
        return applied;
    }

    void RemotePlayerProxyManager::Reset()
    {
        std::scoped_lock lock(g_mutex);
        g_order.clear();
        g_pending.clear();
        for (auto& [id, proxy] : g_proxies) DestroyProxy(id, proxy, "reset");
        g_proxies.clear();
        logs::info("[REMOTE PLAYER PROXY] reset maxNativeProxies={}", kMaxNativeProxies);
    }
}
