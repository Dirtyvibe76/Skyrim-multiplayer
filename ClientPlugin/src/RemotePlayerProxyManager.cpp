#include "pch.h"

#include "RemotePlayerProxyManager.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        // One client never renders its own authoritative player entity, so this
        // supports every remote participant on the server's default 64-player cap.
        constexpr std::size_t kMaxNativeProxies = 63;

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
            bool initialized{};
            std::uint64_t lastRevision{};
            std::uint32_t cellFormId{};
            std::uint32_t worldspaceFormId{};
            std::uint16_t actionFlags{};
            std::vector<std::uint32_t> equippedFormIds;
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
                if (command.kind == ProxyCommandKind::Despawn) {
                    existing->second = command;
                    return;
                }
                if (existing->second.kind == ProxyCommandKind::Despawn) return;
                if (command.update.revision >= existing->second.update.revision) existing->second = command;
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

        void ApplyActorState(RE::Actor& actor, const RemotePlayerProxyUpdate& update)
        {
            if (update.hasActorState) {
                actor.SetActorValue(RE::ActorValue::kHealth, update.health);
                actor.SetActorValue(RE::ActorValue::kMagicka, update.magicka);
                actor.SetActorValue(RE::ActorValue::kStamina, update.stamina);
            }
            if (update.hasStatusState) {
                if (update.dead && !actor.IsDead()) actor.KillImpl(nullptr, 0.0f, false, false);
                if (!update.dead && actor.IsDead()) actor.Resurrect(false, true);
            }
        }

        void ApplyEquipment(RE::Actor& actor, NativeProxy& proxy, const RemotePlayerProxyUpdate& update)
        {
            auto* manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) return;
            for (const auto formId : proxy.equippedFormIds) {
                if (std::binary_search(update.equippedFormIds.begin(), update.equippedFormIds.end(), formId)) continue;
                if (auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formId)) {
                    manager->UnequipObject(&actor, object, nullptr, 1, nullptr, false, true, false, true);
                }
            }
            for (const auto formId : update.equippedFormIds) {
                if (std::binary_search(proxy.equippedFormIds.begin(), proxy.equippedFormIds.end(), formId)) continue;
                if (auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formId)) {
                    manager->EquipObject(&actor, object, nullptr, 1, nullptr, false, true, false, true);
                }
            }
            proxy.equippedFormIds = update.equippedFormIds;
        }

        void ApplyTransform(RE::Actor& actor, const RemotePlayerProxyUpdate& update)
        {
            // This code runs from the main-thread proxy queue and already owns a
            // validated actor pointer.  Do not round-trip through TESForm lookup:
            // newly placed dynamic refs are not reliably discoverable by form ID
            // during their first frames.
            actor.SetPosition(RE::NiPoint3{ update.position.x, update.position.y, update.position.z }, true);
            actor.data.angle.x = update.rotation.x;
            actor.data.angle.y = update.rotation.y;
            actor.data.angle.z = update.rotation.z;
        }

        void InitializeVisualOnlyProxy(RE::Actor& actor, std::uint64_t networkEntityId)
        {
            // A placed ActorBase is only a temporary visual stand-in.  It must
            // never participate in Skyrim gameplay: animation graph commands,
            // inventory/equipment transactions, actor values, death handling,
            // and AI all touch state that belongs to a real actor and caused
            // both clients to crash when the stand-ins met or were activated.
            actor.SetTemporary();
            actor.SetActivationBlocked(true);
            actor.SetCollision(false);
            actor.EnableAI(false);
            logs::info("[REMOTE PLAYER PROXY READY] networkId={:016X} form={:08X} mode=visual-only",
                networkEntityId, actor.GetFormID());
        }

        void ApplyActions(RE::Actor& actor, NativeProxy& proxy, const RemotePlayerProxyUpdate& update)
        {
            const auto changed = static_cast<std::uint16_t>(proxy.actionFlags ^ update.actionFlags);
            if (changed == 0) return;
            const auto enabled = [&](PlayerActionFlag flag) {
                return (update.actionFlags & static_cast<std::uint16_t>(flag)) != 0;
            };
            const auto transitioned = [&](PlayerActionFlag flag) {
                return (changed & static_cast<std::uint16_t>(flag)) != 0;
            };

            if (transitioned(kWeaponDrawn)) actor.DrawWeaponMagicHands(enabled(kWeaponDrawn));
            if (transitioned(kSneaking)) actor.NotifyAnimationGraph(enabled(kSneaking) ? "SneakStart" : "SneakStop");
            if (transitioned(kJumping) && enabled(kJumping)) actor.NotifyAnimationGraph("JumpStandingStart");
            if (transitioned(kAttacking) && enabled(kAttacking)) actor.NotifyAnimationGraph("attackStart");
            if (transitioned(kBlocking)) actor.NotifyAnimationGraph(enabled(kBlocking) ? "blockStart" : "blockStop");
            if (transitioned(kCasting)) actor.NotifyAnimationGraph(enabled(kCasting) ? "MRh_SpellAimedStart" : "MRh_SpellAimedStop");

            proxy.actionFlags = update.actionFlags;
            logs::info("[REMOTE PLAYER ACTION] form={:08X} flags={:04X} changed={:04X} revision={}",
                actor.GetFormID(), update.actionFlags, changed, update.revision);
        }

        void DestroyProxy(std::uint64_t networkEntityId, NativeProxy& proxy, const char* reason)
        {
            if (auto* actor = ResolveActor(proxy); actor && actor->Get3D()) {
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

            NativeProxy proxy;
            proxy.handle = actor->GetHandle();
            proxy.lastRevision = update.revision;
            proxy.cellFormId = update.cellFormId;
            proxy.worldspaceFormId = update.worldspaceFormId;
            const auto [proxyIt, inserted] = g_proxies.emplace(networkEntityId, std::move(proxy));
            if (!inserted) return false;

            logs::info(
                "[REMOTE PLAYER PROXY SPAWN] networkId={:016X} form={:08X} cell={:08X} world={:08X} revision={} active={}/{}",
                networkEntityId,
                actor->GetFormID(),
                update.cellFormId,
                update.worldspaceFormId,
                update.revision,
                g_proxies.size(),
                kMaxNativeProxies);

            // PlaceObjectAtMe returns before the actor's 3D can exist.  Calling
            // inventory, animation, or actor-value APIs in that window caused a
            // two-client crash.  Keep the handle and initialize on a later
            // game-thread update only after Skyrim reports a loaded 3D.
            if (!actor->Get3D()) {
                logs::info("[REMOTE PLAYER PROXY PENDING] networkId={:016X} form={:08X} reason=waiting-for-3D",
                    networkEntityId, actor->GetFormID());
                return true;
            }
            proxyIt->second.initialized = true;
            InitializeVisualOnlyProxy(*actor, networkEntityId);
            ApplyTransform(*actor, update);
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
                // Streaming has not completed.  Never churn the native ref or
                // invoke actor APIs until it is ready.
                logs::debug("[REMOTE PLAYER PROXY WAIT] networkId={:016X} revision={} reason=actor-3D-unavailable",
                    update.networkEntityId, update.revision);
                return;
            }

            if (!proxy.initialized) {
                proxy.initialized = true;
                InitializeVisualOnlyProxy(*actor, update.networkEntityId);
            }

            proxy.lastRevision = update.revision;
            ApplyTransform(*actor, update);
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
