#include "pch.h"

#include "RemotePlayerProxyManager.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SkyrimMP
{
    namespace
    {
        constexpr std::size_t kMaxNativeProxies = 63;
        constexpr float kRetiredProxyDepth = 100000.0f;

        enum class ProxyCommandKind : std::uint8_t
        {
            Upsert,
            Appearance,
            Despawn
        };

        struct ProxyCommand
        {
            ProxyCommandKind kind{ ProxyCommandKind::Upsert };
            RemotePlayerProxyUpdate update;
            PlayerAppearance appearance;
            std::uint64_t networkEntityId{};
        };

        struct NativeProxy
        {
            RE::ObjectRefHandle handle;
            RE::TESNPC* dynamicBase{};
            bool initialized{};
            std::uint64_t lastRevision{};
            std::uint64_t appearanceRevision{};
            std::uint32_t cellFormId{};
            std::uint32_t worldspaceFormId{};
        };

        std::mutex g_mutex;
        std::deque<std::uint64_t> g_order;
        std::unordered_map<std::uint64_t, ProxyCommand> g_pending;
        std::unordered_map<std::uint64_t, NativeProxy> g_proxies;
        std::unordered_map<std::uint64_t, PlayerAppearance> g_appearances;

        // Native actor retirement remains intentionally conservative. Skyrim
        // previously terminated immediately after Disable()+handle release on a
        // remote-player despawn. Retain both reference and duplicated NPC base
        // until process shutdown while the safe destruction path is investigated.
        std::vector<NativeProxy> g_retiredProxies;

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
                if (command.kind == ProxyCommandKind::Appearance) {
                    // Appearance must not be lost behind a transform upsert. Cache
                    // it immediately; the queued transform can then consume it.
                    const auto appearanceIt = g_appearances.find(command.networkEntityId);
                    if (appearanceIt == g_appearances.end() || command.appearance.revision >= appearanceIt->second.revision) {
                        g_appearances[command.networkEntityId] = command.appearance;
                    }
                    return;
                }
                if (existing->second.kind == ProxyCommandKind::Appearance ||
                    command.update.revision >= existing->second.update.revision) {
                    existing->second = std::move(command);
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

        bool LocalContextMatches(const RemotePlayerProxyUpdate& update, RE::PlayerCharacter* player)
        {
            if (!player || update.cellFormId == 0) return false;
            auto* cell = player->GetParentCell();
            if (!cell || cell->GetFormID() != update.cellFormId) return false;

            std::uint32_t localWorld = 0;
            if (auto* world = cell->GetRuntimeData().worldSpace) localWorld = world->GetFormID();
            return localWorld == update.worldspaceFormId;
        }

        RE::TESNPC* SelectSafeAvatarBase(
            RE::PlayerCharacter& player,
            std::uint64_t networkEntityId,
            const PlayerAppearance* appearance)
        {
            RE::TESRace* race = nullptr;
            std::size_t sex = 0;

            if (appearance && appearance->valid && appearance->sex <= 1 && appearance->raceFormId != 0) {
                race = RE::TESForm::LookupByID<RE::TESRace>(appearance->raceFormId);
                sex = appearance->sex;
            }

            if (!race) {
                race = player.GetRace();
                auto* playerBase = player.GetActorBase();
                if (!race || !playerBase) return nullptr;
                sex = static_cast<std::size_t>(playerBase->GetSex());
            }

            if (sex > 1) return nullptr;
            auto* faceData = race->faceRelatedData[sex];
            if (!faceData || !faceData->presetNPCs || faceData->presetNPCs->empty()) return nullptr;

            auto& presets = *faceData->presetNPCs;
            const auto count = presets.size();
            const auto first = static_cast<std::size_t>(networkEntityId % count);
            for (std::size_t offset = 0; offset < count; ++offset) {
                if (auto* preset = presets[(first + offset) % count]) return preset;
            }
            return nullptr;
        }

        bool ApplyAppearanceToBase(
            RE::TESNPC& base,
            const PlayerAppearance& appearance,
            std::uint64_t networkEntityId)
        {
            if (!appearance.valid || appearance.raceFormId == 0 || appearance.sex > 1) {
                logs::warn(
                    "[APPEARANCE-FAIL] networkId={:016X} revision={:016X} reason=invalid profile",
                    networkEntityId,
                    appearance.revision);
                return false;
            }

            auto* race = RE::TESForm::LookupByID<RE::TESRace>(appearance.raceFormId);
            if (!race) {
                logs::warn(
                    "[APPEARANCE-FAIL] networkId={:016X} revision={:016X} race={:08X} reason=race unresolved",
                    networkEntityId,
                    appearance.revision,
                    appearance.raceFormId);
                return false;
            }

            base.race = race;
            base.originalRace = race;
            base.SetActorBaseFlag(RE::ACTOR_BASE_DATA::Flag::kFemale, appearance.sex == 1, false);
            base.SetActorBaseFlag(RE::ACTOR_BASE_DATA::Flag::kNoActivation, true, false);
            base.weight = std::clamp(appearance.weight, 0.0f, 100.0f);
            if (!appearance.displayName.empty()) base.SetFullName(appearance.displayName.c_str());

            base.bodyTintColor.red = static_cast<std::uint8_t>(appearance.bodyTintColor & 0xFFu);
            base.bodyTintColor.green = static_cast<std::uint8_t>((appearance.bodyTintColor >> 8) & 0xFFu);
            base.bodyTintColor.blue = static_cast<std::uint8_t>((appearance.bodyTintColor >> 16) & 0xFFu);

            if (appearance.hairColorFormId != 0) {
                if (auto* hairColor = RE::TESForm::LookupByID<RE::BGSColorForm>(appearance.hairColorFormId)) {
                    base.SetHairColor(hairColor);
                } else {
                    logs::warn(
                        "[APPEARANCE-FAIL] networkId={:016X} revision={:016X} hairColor={:08X} reason=hair color unresolved",
                        networkEntityId,
                        appearance.revision,
                        appearance.hairColorFormId);
                }
            }

            if (appearance.faceDetailsFormId != 0) {
                if (auto* faceDetails = RE::TESForm::LookupByID<RE::BGSTextureSet>(appearance.faceDetailsFormId)) {
                    base.SetFaceTexture(faceDetails);
                }
            }

            for (const auto formId : appearance.headPartFormIds) {
                if (auto* headPart = RE::TESForm::LookupByID<RE::BGSHeadPart>(formId)) {
                    base.ChangeHeadPart(headPart);
                } else {
                    logs::warn(
                        "[APPEARANCE-FAIL] networkId={:016X} revision={:016X} headPart={:08X} reason=head part unresolved",
                        networkEntityId,
                        appearance.revision,
                        formId);
                }
            }

            if (base.faceData) {
                for (std::size_t i = 0; i < appearance.faceMorphs.size(); ++i) base.faceData->morphs[i] = appearance.faceMorphs[i];
                for (std::size_t i = 0; i < appearance.faceParts.size(); ++i) base.faceData->parts[i] = appearance.faceParts[i];
            }

            logs::info(
                "[APPEARANCE-APPLY] networkId={:016X} revision={:016X} base={:08X} name={} race={:08X} sex={} weight={:.3f} headParts={}",
                networkEntityId,
                appearance.revision,
                base.GetFormID(),
                appearance.displayName,
                appearance.raceFormId,
                appearance.sex,
                appearance.weight,
                appearance.headPartFormIds.size());
            return true;
        }

        RE::TESNPC* CreateDynamicAvatarBase(
            RE::PlayerCharacter& player,
            std::uint64_t networkEntityId,
            const PlayerAppearance* appearance)
        {
            auto* preset = SelectSafeAvatarBase(player, networkEntityId, appearance);
            if (!preset) return nullptr;

            auto* duplicateForm = preset->CreateDuplicateForm(false, nullptr);
            auto* duplicate = duplicateForm ? duplicateForm->As<RE::TESNPC>() : nullptr;
            if (!duplicate) {
                logs::warn(
                    "[APPEARANCE-FAIL] networkId={:016X} reason=NPC base duplication failed preset={:08X}",
                    networkEntityId,
                    preset->GetFormID());
                return nullptr;
            }

            duplicate->SetActorBaseFlag(RE::ACTOR_BASE_DATA::Flag::kNoActivation, true, false);
            if (appearance && appearance->valid) ApplyAppearanceToBase(*duplicate, *appearance, networkEntityId);
            return duplicate;
        }

        void InitializeVisualOnlyProxy(RE::Actor& actor, std::uint64_t networkEntityId, std::uint32_t baseFormId)
        {
            actor.SetTemporary();
            actor.SetActivationBlocked(true);
            actor.SetCollision(false);
            actor.EnableAI(false);
            logs::info(
                "[REMOTE PLAYER AVATAR READY] networkId={:016X} form={:08X} base={:08X} mode=npc-visual-shell",
                networkEntityId,
                actor.GetFormID(),
                baseFormId);
        }

        void ApplyTransform(RE::Actor& actor, const RemotePlayerProxyUpdate& update)
        {
            actor.SetPosition(RE::NiPoint3{ update.position.x, update.position.y, update.position.z }, true);
            actor.data.angle.x = update.rotation.x;
            actor.data.angle.y = update.rotation.y;
            actor.data.angle.z = update.rotation.z;
            actor.Update3DPosition(true);
        }

        void QuarantineProxy(std::uint64_t networkEntityId, NativeProxy proxy, const char* reason)
        {
            std::uint32_t formId = 0;
            if (auto* actor = ResolveActor(proxy)) {
                formId = actor->GetFormID();
                actor->SetActivationBlocked(true);
                actor->EnableAI(false);
                actor->SetCollision(false);

                const auto position = actor->GetPosition();
                actor->SetPosition(RE::NiPoint3{
                    position.x,
                    position.y,
                    position.z - kRetiredProxyDepth
                }, true);
                actor->Update3DPosition(true);
            }

            g_retiredProxies.push_back(std::move(proxy));
            logs::info(
                "[REMOTE PLAYER AVATAR RETIRED] networkId={:016X} form={:08X} reason={} retained={} nativeDisable=false",
                networkEntityId,
                formId,
                reason,
                g_retiredProxies.size());
        }

        void RetireActiveProxy(std::uint64_t networkEntityId, const char* reason)
        {
            const auto it = g_proxies.find(networkEntityId);
            if (it == g_proxies.end()) return;
            NativeProxy proxy = std::move(it->second);
            g_proxies.erase(it);
            QuarantineProxy(networkEntityId, std::move(proxy), reason);
        }

        bool SpawnProxy(std::uint64_t networkEntityId, const RemotePlayerProxyUpdate& update)
        {
            if (g_proxies.contains(networkEntityId)) return true;

            if (g_proxies.size() >= kMaxNativeProxies) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=controlled proxy limit limit={}", networkEntityId, kMaxNativeProxies);
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!LocalContextMatches(update, player)) return false;

            const PlayerAppearance* appearance = nullptr;
            const auto appearanceIt = g_appearances.find(networkEntityId);
            if (appearanceIt != g_appearances.end()) appearance = &appearanceIt->second;

            auto* avatarBase = CreateDynamicAvatarBase(*player, networkEntityId, appearance);
            if (!avatarBase) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=no safe unique NPC avatar base", networkEntityId);
                return false;
            }

            auto* cell = player->GetParentCell();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!cell || !handler) return false;
            auto* world = cell->GetRuntimeData().worldSpace;

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
            proxy.dynamicBase = avatarBase;
            proxy.lastRevision = update.revision;
            proxy.appearanceRevision = appearance ? appearance->revision : 0;
            proxy.cellFormId = update.cellFormId;
            proxy.worldspaceFormId = update.worldspaceFormId;
            const auto [proxyIt, inserted] = g_proxies.emplace(networkEntityId, std::move(proxy));
            if (!inserted) {
                logs::warn("[REMOTE PLAYER DROP] networkId={:016X} reason=duplicate active proxy after native create", networkEntityId);
                NativeProxy orphan;
                orphan.handle = actor->GetHandle();
                orphan.dynamicBase = avatarBase;
                QuarantineProxy(networkEntityId, std::move(orphan), "duplicate native create");
                return false;
            }

            logs::info(
                "[REMOTE PLAYER AVATAR SPAWN] networkId={:016X} form={:08X} base={:08X} cell={:08X} world={:08X} revision={} appearanceRevision={:016X} active={}/{}",
                networkEntityId,
                actor->GetFormID(),
                avatarBase->GetFormID(),
                update.cellFormId,
                update.worldspaceFormId,
                update.revision,
                proxyIt->second.appearanceRevision,
                g_proxies.size(),
                kMaxNativeProxies);

            actor->Enable(false);
            ApplyTransform(*actor, update);

            if (!actor->Get3D()) {
                logs::info(
                    "[REMOTE PLAYER AVATAR PENDING] networkId={:016X} form={:08X} reason=waiting-for-3D-after-enable",
                    networkEntityId,
                    actor->GetFormID());
                return true;
            }

            proxyIt->second.initialized = true;
            InitializeVisualOnlyProxy(*actor, networkEntityId, avatarBase->GetFormID());
            return true;
        }

        void ApplyAppearance(std::uint64_t networkEntityId, const PlayerAppearance& appearance)
        {
            const auto cached = g_appearances.find(networkEntityId);
            if (cached != g_appearances.end() && appearance.revision < cached->second.revision) return;
            g_appearances[networkEntityId] = appearance;

            auto proxyIt = g_proxies.find(networkEntityId);
            if (proxyIt == g_proxies.end()) {
                logs::info(
                    "[APPEARANCE-RECEIVE] networkId={:016X} revision={:016X} state=cached-before-spawn",
                    networkEntityId,
                    appearance.revision);
                return;
            }

            auto& proxy = proxyIt->second;
            if (appearance.revision <= proxy.appearanceRevision || !proxy.dynamicBase) return;
            if (!ApplyAppearanceToBase(*proxy.dynamicBase, appearance, networkEntityId)) return;

            proxy.appearanceRevision = appearance.revision;
            if (auto* actor = ResolveActor(proxy)) {
                actor->SetActivationBlocked(true);
                actor->EnableAI(false);
                actor->SetCollision(false);
                actor->DoReset3D(true);
                actor->SetActivationBlocked(true);
                actor->EnableAI(false);
                actor->SetCollision(false);
            }
        }

        void ApplyUpsert(const RemotePlayerProxyUpdate& update)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!LocalContextMatches(update, player)) {
                RetireActiveProxy(update.networkEntityId, "local context changed");
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
                RetireActiveProxy(update.networkEntityId, "remote context changed");
                SpawnProxy(update.networkEntityId, update);
                return;
            }

            auto* actor = ResolveActor(proxy);
            if (!actor) {
                logs::debug(
                    "[REMOTE PLAYER AVATAR WAIT] networkId={:016X} revision={} reason=actor-unavailable",
                    update.networkEntityId,
                    update.revision);
                return;
            }

            if (!actor->Get3D()) {
                actor->Enable(false);
                ApplyTransform(*actor, update);
                if (!actor->Get3D()) {
                    logs::debug(
                        "[REMOTE PLAYER AVATAR WAIT] networkId={:016X} revision={} reason=actor-3D-unavailable-after-enable",
                        update.networkEntityId,
                        update.revision);
                    return;
                }
            }

            if (!proxy.initialized) {
                proxy.initialized = true;
                const auto baseFormId = actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0u;
                InitializeVisualOnlyProxy(*actor, update.networkEntityId, baseFormId);
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

    void RemotePlayerProxyManager::EnqueueAppearance(std::uint64_t networkEntityId, const PlayerAppearance& appearance)
    {
        if (networkEntityId == 0 || !appearance.valid || appearance.revision == 0) return;
        std::scoped_lock lock(g_mutex);
        ProxyCommand command;
        command.kind = ProxyCommandKind::Appearance;
        command.networkEntityId = networkEntityId;
        command.appearance = appearance;
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
                if (g_proxies.contains(command.networkEntityId)) {
                    RetireActiveProxy(command.networkEntityId, "server despawn");
                    ++applied;
                }
                g_appearances.erase(command.networkEntityId);
                continue;
            }
            if (command.kind == ProxyCommandKind::Appearance) {
                ApplyAppearance(command.networkEntityId, command.appearance);
                ++applied;
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
        g_appearances.clear();

        std::vector<std::uint64_t> activeIds;
        activeIds.reserve(g_proxies.size());
        for (const auto& [id, proxy] : g_proxies) {
            (void)proxy;
            activeIds.push_back(id);
        }
        for (const auto id : activeIds) RetireActiveProxy(id, "reset");

        logs::info(
            "[REMOTE PLAYER AVATAR] reset active={} retained={} maxNativeProxies={} nativeDisable=false",
            g_proxies.size(),
            g_retiredProxies.size(),
            kMaxNativeProxies);
    }
}
