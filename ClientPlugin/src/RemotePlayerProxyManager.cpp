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
        // The local client never renders its own authoritative entity.
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
            std::uint32_t raceFormId{};
            std::uint8_t sex{};
            std::uint64_t appearanceSeed{};
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
                    existing->second = std::move(command);
                    return;
                }
                if (existing->second.kind == ProxyCommandKind::Despawn) return;
                if (command.update.revision >= existing->second.update.revision) existing->second = std::move(command);
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

        bool LocalContextMatches(const RemotePlayerProxyUpdate& update, RE::PlayerCharacter* player)
        {
            if (!player || update.cellFormId == 0) return false;
            auto* cell = player->GetParentCell();
            if (!cell || cell->GetFormID() != update.cellFormId) return false;

            std::uint32_t localWorld = 0;
            if (auto* world = cell->GetRuntimeData().worldSpace) localWorld = world->GetFormID();
            return localWorld == update.worldspaceFormId;
        }

        RE::TESNPC* SelectAvatarBase(const RemotePlayerProxyUpdate& update)
        {
            if (!update.appearance.valid || update.appearance.raceFormId == 0 || update.appearance.sex > 1) return nullptr;

            auto* race = RE::TESForm::LookupByID<RE::TESRace>(update.appearance.raceFormId);
            if (!race || !race->GetPlayable()) return nullptr;

            auto* faceData = race->faceRelatedData[update.appearance.sex];
            if (!faceData || !faceData->presetNPCs || faceData->presetNPCs->empty()) return nullptr;

            auto& presets = *faceData->presetNPCs;
            const auto count = presets.size();
            const auto seed = update.appearance.appearanceSeed != 0 ? update.appearance.appearanceSeed : update.networkEntityId;
            const auto first = static_cast<std::size_t>(seed % count);
            for (std::size_t offset = 0; offset < count; ++offset) {
                if (auto* preset = presets[(first + offset) % count]) return preset;
            }
            return nullptr;
        }

        void InitializeVisualOnlyProxy(RE::Actor& actor, const RemotePlayerProxyUpdate& update)
        {
            // Remote players are normal NPC-backed references, never clones of
            // PlayerCharacter. Keep Skyrim gameplay systems off the shell until
            // explicit replicated interaction handling is implemented.
            actor.SetTemporary();
            actor.SetActivationBlocked(true);
            actor.SetCollision(false);
            actor.EnableAI(false);
            logs::info(
                "[REMOTE PLAYER AVATAR READY] networkId={:016X} form={:08X} race={:08X} sex={} seed={:016X} mode=npc-visual-shell",
                update.networkEntityId,
                actor.GetFormID(),
                update.appearance.raceFormId,
                update.appearance.sex,
                update.appearance.appearanceSeed);
        }

        void ApplyTransform(RE::Actor& actor, const RemotePlayerProxyUpdate& update)
        {
            actor.SetPosition(RE::NiPoint3{ update.position.x, update.position.y, update.position.z }, true);
            actor.data.angle.x = update.rotation.x;
            actor.data.angle.y = update.rotation.y;
            actor.data.angle.z = update.rotation.z;
            actor.Update3DPosition(true);
        }

        void DestroyProxy(std::uint64_t networkEntityId, NativeProxy& proxy, const char* reason)
        {
            if (auto* actor = ResolveActor(proxy)) {
                actor->SetActivationBlocked(true);
                actor->EnableAI(false);
                actor->SetCollision(false);
                actor->Disable();
            }
            logs::info("[REMOTE PLAYER AVATAR DESPAWN] networkId={:016X} reason={}", networkEntityId, reason);
        }

        bool AppearanceChanged(const NativeProxy& proxy, const RemotePlayerProxyUpdate& update)
        {
            return proxy.raceFormId != update.appearance.raceFormId ||
                proxy.sex != update.appearance.sex ||
                proxy.appearanceSeed != update.appearance.appearanceSeed;
        }

        bool SpawnProxy(std::uint64_t networkEntityId, const RemotePlayerProxyUpdate& update)
        {
            if (g_proxies.size() >= kMaxNativeProxies) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=controlled proxy limit limit={}", networkEntityId, kMaxNativeProxies);
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!LocalContextMatches(update, player)) return false;

            auto* avatarBase = SelectAvatarBase(update);
            if (!avatarBase) {
                logs::warn(
                    "[REMOTE PLAYER DROP] networkId={:016X} reason=no safe race preset race={:08X} sex={} appearanceValid={}",
                    networkEntityId,
                    update.appearance.raceFormId,
                    update.appearance.sex,
                    update.appearance.valid);
                return false;
            }

            auto* cell = player->GetParentCell();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!cell || !handler) return false;
            auto* world = cell->GetRuntimeData().worldSpace;

            // Create the reference directly at the authoritative remote transform.
            // The old path called player->PlaceObjectAtMe(player->GetActorBase()),
            // which both cloned PlayerCharacter and briefly created that clone on
            // top of the local player. Contact with that invalid actor crashed.
            const RE::NiPoint3 position{ update.position.x, update.position.y, update.position.z };
            const RE::NiPoint3 rotation{ update.rotation.x, update.rotation.y, update.rotation.z };
            const auto handle = handler->CreateReferenceAtLocation(
                avatarBase,
                position,
                rotation,
                cell,
                world,
                nullptr,
                nullptr,
                RE::ObjectRefHandle(),
                false,
                true);
            auto reference = handle.get();
            auto* actor = reference ? reference->As<RE::Actor>() : nullptr;
            if (!actor) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=CreateReferenceAtLocation failed", networkEntityId);
                return false;
            }

            NativeProxy proxy;
            proxy.handle = actor->GetHandle();
            proxy.lastRevision = update.revision;
            proxy.cellFormId = update.cellFormId;
            proxy.worldspaceFormId = update.worldspaceFormId;
            proxy.raceFormId = update.appearance.raceFormId;
            proxy.sex = update.appearance.sex;
            proxy.appearanceSeed = update.appearance.appearanceSeed;
            const auto [proxyIt, inserted] = g_proxies.emplace(networkEntityId, std::move(proxy));
            if (!inserted) {
                actor->Disable();
                return false;
            }

            logs::info(
                "[REMOTE PLAYER AVATAR SPAWN] networkId={:016X} form={:08X} base={:08X} cell={:08X} world={:08X} revision={} active={}/{}",
                networkEntityId,
                actor->GetFormID(),
                avatarBase->GetFormID(),
                update.cellFormId,
                update.worldspaceFormId,
                update.revision,
                g_proxies.size(),
                kMaxNativeProxies);

            if (!actor->Get3D()) {
                logs::info("[REMOTE PLAYER AVATAR PENDING] networkId={:016X} form={:08X} reason=waiting-for-3D",
                    networkEntityId, actor->GetFormID());
                return true;
            }

            proxyIt->second.initialized = true;
            InitializeVisualOnlyProxy(*actor, update);
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

            if (proxy.cellFormId != update.cellFormId ||
                proxy.worldspaceFormId != update.worldspaceFormId ||
                AppearanceChanged(proxy, update)) {
                DestroyProxy(update.networkEntityId, proxy,
                    AppearanceChanged(proxy, update) ? "appearance changed" : "remote context changed");
                g_proxies.erase(proxyIt);
                SpawnProxy(update.networkEntityId, update);
                return;
            }

            auto* actor = ResolveActor(proxy);
            if (!actor || !actor->Get3D()) {
                logs::debug("[REMOTE PLAYER AVATAR WAIT] networkId={:016X} revision={} reason=actor-3D-unavailable",
                    update.networkEntityId, update.revision);
                return;
            }

            if (!proxy.initialized) {
                proxy.initialized = true;
                InitializeVisualOnlyProxy(*actor, update);
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
        logs::info("[REMOTE PLAYER AVATAR] reset maxNativeProxies={}", kMaxNativeProxies);
    }
}
