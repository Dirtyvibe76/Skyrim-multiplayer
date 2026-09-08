#include "pch.h"

#include "WorldStateClient.h"
#include "NativeEntityRegistry.h"
#include "WorldEntityId.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace SkyrimMP
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr std::uint32_t kWorldMagic = 0x31535753u; // "SWS1"
        constexpr std::uint16_t kWorldProtocol = 3;
        constexpr auto kLoadOrderRevision = "7dc35a831945468b790a6b3398236c0fe9fe7c8b32425be9ef07ca1434d6c808";
        constexpr std::size_t kMaxDatagram = 1200;
        constexpr auto kPickupCorrelationWindow = 5s;

        enum class WorldPacketKind : std::uint8_t
        {
            Hello = 1,
            Welcome = 2,
            Observation = 3,
            Snapshot = 4,
            Reject = 5,
            DropRequest = 6,
            DropSnapshot = 7,
            RemoveDynamic = 8
        };

        struct CanonicalKey
        {
            bool light{};
            std::uint32_t namespaceIndex{};
            std::uint32_t localId{};
        };

        struct ReferenceState
        {
            bool enabled{ true };
            bool open{};
            bool removed{};
            bool hasOpenState{};
        };

        struct PendingObservation
        {
            ReferenceState state;
            std::chrono::steady_clock::time_point lastSent{};
        };

        struct AuthoritativeState
        {
            ReferenceState state;
            std::uint64_t revision{};
            std::uint32_t runtimeFormId{};
        };

        struct RecentActivation
        {
            std::uint32_t referenceFormId{};
            std::uint32_t baseObjectFormId{};
            WorldEntityId worldEntityId{};
            std::chrono::steady_clock::time_point capturedAt{};
        };

        struct PendingDropRequest
        {
            std::uint64_t requestId{};
            RE::ObjectRefHandle localHandle;
            CanonicalKey baseObject;
            CanonicalKey cell;
            CanonicalKey worldspace;
            RE::NiPoint3 position;
            RE::NiPoint3 rotation;
            std::uint16_t count{ 1 };
            bool exterior{};
            bool hasWorldspace{};
            std::chrono::steady_clock::time_point lastSent{};
        };

        struct PendingDropIntent
        {
            std::uint64_t requestId{};
            std::uint32_t baseObjectFormId{};
            std::uint16_t count{ 1 };
            std::chrono::steady_clock::time_point capturedAt{};
            std::chrono::steady_clock::time_point lastProbeScheduled{};
        };

        struct PendingDynamicRemoval
        {
            std::uint64_t requestId{};
            WorldEntityId entityId{};
            std::uint32_t baseObjectFormId{};
            std::uint16_t count{ 1 };
            std::chrono::steady_clock::time_point lastSent{};
        };

        struct DynamicDropSnapshot
        {
            std::uint64_t requestId{};
            WorldEntityId entityId{};
            std::uint32_t baseObjectFormId{};
            std::uint32_t cellFormId{};
            std::uint32_t worldspaceFormId{};
            RE::NiPoint3 position;
            RE::NiPoint3 rotation;
            std::uint16_t count{ 1 };
            std::uint64_t revision{};
            bool exterior{};
            bool removed{};
            bool pickupAccepted{};
            bool rollbackPickup{};
            std::uint16_t rollbackCount{};
            RE::ObjectRefHandle originatingHandle;
        };

        struct ServerTarget
        {
            std::string address{ "127.0.0.1" };
            std::uint16_t port{ 10578 };
        };

        std::atomic_bool g_running{ false };
        std::atomic_bool g_applyingAuthoritative{ false };
        std::atomic_bool g_sinkInstalled{ false };
        std::jthread g_thread;
        std::mutex g_stateMutex;
        std::unordered_map<std::uint32_t, PendingObservation> g_pending;
        std::unordered_map<WorldEntityId, AuthoritativeState> g_authoritative;
        std::optional<RecentActivation> g_recentActivation;
        std::unordered_map<std::uint64_t, PendingDropRequest> g_pendingDrops;
        std::unordered_map<std::uint64_t, PendingDropIntent> g_pendingDropIntents;
        std::unordered_map<std::uint64_t, PendingDynamicRemoval> g_pendingDynamicRemovals;
        std::atomic_uint64_t g_nextTransaction{ 1 };

        std::string Trim(std::string value)
        {
            const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
                return !isSpace(static_cast<unsigned char>(c));
            }));
            value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
                return !isSpace(static_cast<unsigned char>(c));
            }).base(), value.end());
            return value;
        }

        std::uint64_t NewTransactionId()
        {
            static std::random_device random;
            const auto sequence = g_nextTransaction.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t id = (static_cast<std::uint64_t>(random()) << 32) ^
                static_cast<std::uint64_t>(random()) ^ (GetTickCount64() << 1) ^ sequence;
            return id != 0 ? id : sequence;
        }

        ServerTarget ReadServerTarget()
        {
            ServerTarget target;
            std::ifstream input("Data/SKSE/Plugins/SkyrimMPClient.ini");
            std::string line;
            while (std::getline(input, line)) {
                const auto equals = line.find('=');
                if (equals == std::string::npos) continue;
                const auto key = Trim(line.substr(0, equals));
                const auto value = Trim(line.substr(equals + 1));
                if (key == "Address" && !value.empty()) target.address = value;
                if (key == "Port") {
                    try {
                        const auto parsed = std::stoul(value);
                        if (parsed > 0 && parsed <= 65535) target.port = static_cast<std::uint16_t>(parsed);
                    } catch (...) {}
                }
            }
            return target;
        }

        template <class T>
        void Append(std::vector<std::uint8_t>& out, T value)
        {
            using U = std::make_unsigned_t<T>;
            const U u = static_cast<U>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                out.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFFu));
            }
        }

        template <class T>
        T Read(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("world-state packet truncated");
            using U = std::make_unsigned_t<T>;
            U value{};
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value |= static_cast<U>(bytes[offset++]) << (i * 8);
            }
            return static_cast<T>(value);
        }

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalKey& key)
        {
            Append(out, static_cast<std::uint8_t>(key.light ? 1u : 0u));
            Append(out, key.namespaceIndex);
            Append(out, key.localId);
        }

        CanonicalKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            CanonicalKey key;
            const auto kind = Read<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("world-state canonical key invalid");
            key.light = kind != 0;
            key.namespaceIndex = Read<std::uint32_t>(bytes, offset);
            key.localId = Read<std::uint32_t>(bytes, offset);
            return key;
        }

        bool RuntimeFormToCanonical(std::uint32_t formId, CanonicalKey& out)
        {
            if (formId == 0) return false;
            const auto high = static_cast<std::uint8_t>(formId >> 24);
            if (high == 0xFF) return false;
            if (high == 0xFE) {
                out.light = true;
                out.namespaceIndex = (formId >> 12) & 0x0FFFu;
                out.localId = formId & 0x0FFFu;
                return true;
            }
            out.light = false;
            out.namespaceIndex = high;
            out.localId = formId & 0x00FFFFFFu;
            return true;
        }

        std::uint32_t CanonicalToRuntimeForm(const CanonicalKey& key)
        {
            if (key.light) {
                if (key.namespaceIndex > 0x0FFFu || key.localId > 0x0FFFu) return 0;
                return 0xFE000000u | (key.namespaceIndex << 12) | key.localId;
            }
            if (key.namespaceIndex > 0xFDu || key.localId > 0x00FFFFFFu) return 0;
            return (key.namespaceIndex << 24) | key.localId;
        }

        WorldEntityId StaticWorldEntityId(const CanonicalKey& key)
        {
            return MakeStaticWorldEntityId(key.light, key.namespaceIndex, key.localId);
        }

        std::uint8_t EncodeState(const ReferenceState& state)
        {
            std::uint8_t flags = 0;
            if (state.enabled) flags |= 0x01;
            if (state.open) flags |= 0x02;
            if (state.removed) flags |= 0x04;
            if (state.hasOpenState) flags |= 0x08;
            return flags;
        }

        ReferenceState DecodeState(std::uint8_t flags)
        {
            if ((flags & ~0x0Fu) != 0) throw std::runtime_error("world-state flags invalid");
            ReferenceState state;
            state.enabled = (flags & 0x01) != 0;
            state.open = (flags & 0x02) != 0;
            state.removed = (flags & 0x04) != 0;
            state.hasOpenState = (flags & 0x08) != 0;
            if (state.removed && state.enabled) throw std::runtime_error("removed world reference cannot be enabled");
            if (!state.hasOpenState) state.open = false;
            return state;
        }

        bool SameState(const ReferenceState& a, const ReferenceState& b)
        {
            return a.enabled == b.enabled && a.open == b.open && a.removed == b.removed &&
                a.hasOpenState == b.hasOpenState;
        }

        std::vector<std::uint8_t> MakePacket(WorldPacketKind kind, const std::vector<std::uint8_t>& payload = {})
        {
            std::vector<std::uint8_t> out;
            out.reserve(8 + payload.size());
            Append(out, kWorldMagic);
            Append(out, kWorldProtocol);
            Append(out, static_cast<std::uint8_t>(kind));
            Append(out, static_cast<std::uint8_t>(0));
            out.insert(out.end(), payload.begin(), payload.end());
            if (out.size() > kMaxDatagram) throw std::runtime_error("world-state packet exceeds datagram limit");
            return out;
        }

        std::vector<std::uint8_t> MakeHello()
        {
            std::vector<std::uint8_t> payload;
            const std::string revision = kLoadOrderRevision;
            if (revision.size() > 255) throw std::runtime_error("world-state load-order revision too long");
            Append(payload, static_cast<std::uint8_t>(revision.size()));
            payload.insert(payload.end(), revision.begin(), revision.end());
            return MakePacket(WorldPacketKind::Hello, payload);
        }

        std::vector<std::uint8_t> MakeObservation(std::uint32_t runtimeFormId, const ReferenceState& state)
        {
            CanonicalKey key;
            if (!RuntimeFormToCanonical(runtimeFormId, key)) return {};
            std::vector<std::uint8_t> payload;
            Append(payload, StaticWorldEntityId(key));
            AppendKey(payload, key);
            Append(payload, EncodeState(state));
            return MakePacket(WorldPacketKind::Observation, payload);
        }

        std::vector<std::uint8_t> MakeDropRequest(const PendingDropRequest& request)
        {
            std::vector<std::uint8_t> payload;
            Append(payload, request.requestId);
            AppendKey(payload, request.baseObject);
            std::uint8_t flags = 0x02;
            if (request.exterior) flags |= 0x01;
            if (request.hasWorldspace) flags |= 0x04;
            Append(payload, flags);
            AppendKey(payload, request.cell);
            AppendKey(payload, request.worldspace);
            const auto appendFloat = [&](float value) { Append(payload, std::bit_cast<std::uint32_t>(value)); };
            appendFloat(request.position.x);
            appendFloat(request.position.y);
            appendFloat(request.position.z);
            appendFloat(request.rotation.x);
            appendFloat(request.rotation.y);
            appendFloat(request.rotation.z);
            Append(payload, request.count);
            return MakePacket(WorldPacketKind::DropRequest, payload);
        }

        std::vector<std::uint8_t> MakeDynamicRemoval(const PendingDynamicRemoval& request)
        {
            std::vector<std::uint8_t> payload;
            Append(payload, request.requestId);
            Append(payload, request.entityId);
            Append(payload, request.count);
            return MakePacket(WorldPacketKind::RemoveDynamic, payload);
        }

        void SendPacket(SOCKET socketValue, const sockaddr_in& target, const std::vector<std::uint8_t>& bytes)
        {
            if (bytes.empty()) return;
            const auto sent = sendto(
                socketValue,
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                reinterpret_cast<const sockaddr*>(&target),
                sizeof(target));
            if (sent != static_cast<int>(bytes.size()) && WSAGetLastError() != WSAEWOULDBLOCK) {
                logs::warn("[WORLD-STATE] UDP send failed error={}", WSAGetLastError());
            }
        }

        bool RunReferenceCommand(RE::TESObjectREFR* reference, std::string_view command)
        {
            if (!reference) return false;
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
            auto* form = factory ? factory->Create() : nullptr;
            auto* script = form ? form->As<RE::Script>() : nullptr;
            if (!script) return false;
            script->SetCommand(command);
            script->CompileAndRun(reference);
            script->ClearCommand();
            return true;
        }

        void ApplyReferenceState(WorldEntityId entityId, ReferenceState state, std::uint64_t revision)
        {
            auto* reference = NativeEntityRegistry::Resolve(entityId);
            if (!reference || reference->As<RE::Actor>()) return;
            const auto formId = reference->GetFormID();

            g_applyingAuthoritative.store(true, std::memory_order_release);
            if (state.removed || !state.enabled) {
                if (!reference->IsDisabled()) reference->Disable();
            } else {
                if (reference->IsDisabled()) reference->Enable(false);
                if (state.hasOpenState) {
                    if (!RunReferenceCommand(reference, state.open ? "SetOPENstate 1" : "SetOPENstate 0")) {
                        logs::warn("[WORLD-STATE] failed to apply door state form={:08X}", formId);
                    }
                }
            }
            g_applyingAuthoritative.store(false, std::memory_order_release);
            logs::info(
                "[WORLD-STATE-APPLY] worldEntity={:016X} form={:08X} revision={} enabled={} openKnown={} open={} removed={}",
                entityId,
                formId,
                revision,
                state.enabled,
                state.hasOpenState,
                state.open,
                state.removed);
        }

        void ScheduleApply(WorldEntityId entityId)
        {
            AuthoritativeState authoritative;
            {
                std::scoped_lock lock(g_stateMutex);
                const auto it = g_authoritative.find(entityId);
                if (it == g_authoritative.end()) return;
                authoritative = it->second;
            }
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([entityId, authoritative]() {
                    ApplyReferenceState(entityId, authoritative.state, authoritative.revision);
                });
            }
        }

        void QueueObservation(std::uint32_t formId, ReferenceState state)
        {
            if (!g_running.load(std::memory_order_acquire) || formId == 0 ||
                g_applyingAuthoritative.load(std::memory_order_acquire)) return;
            if (state.removed) state.enabled = false;
            if (!state.hasOpenState) state.open = false;

            std::scoped_lock lock(g_stateMutex);
            auto& pending = g_pending[formId];
            if (pending.state.removed && !state.removed) return;
            pending.state = state;
            pending.lastSent = {};
        }

        bool QueueDroppedReference(
            std::uint64_t requestId,
            std::uint32_t baseObjectFormId,
            std::uint16_t count,
            RE::TESObjectREFR* reference)
        {
            if (requestId == 0 || baseObjectFormId == 0 || count == 0 || !reference || reference->As<RE::Actor>()) return false;
            auto* cell = reference->GetParentCell();
            if (!cell) return false;

            PendingDropRequest request;
            request.requestId = requestId;
            if (!RuntimeFormToCanonical(baseObjectFormId, request.baseObject) ||
                !RuntimeFormToCanonical(cell->GetFormID(), request.cell)) return false;
            request.localHandle = reference->GetHandle();
            request.position = reference->GetPosition();
            request.rotation = reference->GetAngle();
            request.count = count;
            if (auto* world = cell->GetRuntimeData().worldSpace) {
                request.exterior = true;
                request.hasWorldspace = RuntimeFormToCanonical(world->GetFormID(), request.worldspace);
                if (!request.hasWorldspace) return false;
            }

            const auto formId = reference->GetFormID();
            {
                std::scoped_lock lock(g_stateMutex);
                if (g_pendingDrops.contains(requestId)) return true;
                g_pendingDropIntents.erase(requestId);
                g_pendingDrops.emplace(requestId, std::move(request));
            }
            logs::info("[WORLD-DROP-REQUEST] request={:016X} localForm={:08X} base={:08X} count={}",
                requestId, formId, baseObjectFormId, count);
            return true;
        }

        void ResolveDropIntentNearPlayer(std::uint64_t requestId)
        {
            PendingDropIntent intent;
            {
                std::scoped_lock lock(g_stateMutex);
                const auto it = g_pendingDropIntents.find(requestId);
                if (it == g_pendingDropIntents.end()) return;
                if (std::chrono::steady_clock::now() - it->second.capturedAt > 2s) {
                    logs::warn("[WORLD-DROP] native reference correlation expired request={:016X} base={:08X}",
                        requestId, it->second.baseObjectFormId);
                    g_pendingDropIntents.erase(it);
                    return;
                }
                intent = it->second;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!player || !cell) return;

            RE::TESObjectREFR* best = nullptr;
            float bestDistance = (std::numeric_limits<float>::max)();
            cell->ForEachReferenceInRange(player->GetPosition(), 768.0F, [&](RE::TESObjectREFR* candidate) {
                if (!candidate || candidate->As<RE::Actor>() || candidate->IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto formId = candidate->GetFormID();
                if ((formId & 0xFF000000u) != 0xFF000000u ||
                    NativeEntityRegistry::FindByRuntimeFormId(formId) != 0) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* baseObject = candidate->GetBaseObject();
                if (!baseObject || baseObject->GetFormID() != intent.baseObjectFormId) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto distance = player->GetPosition().GetSquaredDistance(candidate->GetPosition());
                if (!best || distance < bestDistance ||
                    (distance == bestDistance && formId > best->GetFormID())) {
                    best = candidate;
                    bestDistance = distance;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });

            if (best && QueueDroppedReference(requestId, intent.baseObjectFormId, intent.count, best)) {
                logs::info("[WORLD-DROP-CORRELATED] request={:016X} localForm={:08X} source=cell-scan",
                    requestId, best->GetFormID());
            }
        }

        void ResolveDropIntentFromLoadedReference(std::uint32_t formId)
        {
            if ((formId & 0xFF000000u) != 0xFF000000u ||
                NativeEntityRegistry::FindByRuntimeFormId(formId) != 0) return;
            auto* form = RE::TESForm::LookupByID(formId);
            auto* reference = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!reference || reference->As<RE::Actor>()) return;
            const auto* baseObject = reference->GetBaseObject();
            if (!baseObject) return;

            std::vector<PendingDropIntent> matches;
            const auto now = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock(g_stateMutex);
                for (auto it = g_pendingDropIntents.begin(); it != g_pendingDropIntents.end();) {
                    if (now - it->second.capturedAt > 2s) {
                        it = g_pendingDropIntents.erase(it);
                        continue;
                    }
                    if (it->second.baseObjectFormId == baseObject->GetFormID()) matches.push_back(it->second);
                    ++it;
                }
            }
            std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
                return left.capturedAt < right.capturedAt;
            });
            for (const auto& intent : matches) {
                if (QueueDroppedReference(intent.requestId, intent.baseObjectFormId, intent.count, reference)) {
                    logs::info("[WORLD-DROP-CORRELATED] request={:016X} localForm={:08X} source=object-loaded",
                        intent.requestId, formId);
                    break;
                }
            }
        }

        void QueueDroppedItem(const RE::TESContainerChangedEvent* event, const RE::PlayerCharacter& player)
        {
            if (!event || event->oldContainer != player.GetFormID() || event->newContainer != 0 ||
                event->baseObj == 0 || event->itemCount <= 0 || event->itemCount > 32767) return;

            auto reference = event->reference.get();
            const auto requestId = NewTransactionId();
            const auto count = static_cast<std::uint16_t>(event->itemCount);
            logs::info("[WORLD-DROP-EVENT] request={:016X} old={:08X} new={:08X} base={:08X} count={} ref={:08X} unique={}",
                requestId,
                event->oldContainer,
                event->newContainer,
                event->baseObj,
                event->itemCount,
                reference ? reference->GetFormID() : 0,
                event->uniqueID);

            if (reference && QueueDroppedReference(requestId, event->baseObj, count, reference.get())) return;

            {
                std::scoped_lock lock(g_stateMutex);
                g_pendingDropIntents[requestId] = PendingDropIntent{
                    requestId,
                    event->baseObj,
                    count,
                    std::chrono::steady_clock::now()
                };
            }
            logs::info("[WORLD-DROP-DEFERRED] request={:016X} base={:08X} count={}",
                requestId, event->baseObj, event->itemCount);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([requestId]() { ResolveDropIntentNearPlayer(requestId); });
            }
        }

        void QueueDynamicRemoval(WorldEntityId entityId, std::uint32_t baseObjectFormId, std::uint16_t count)
        {
            if (!IsDynamicWorldEntity(entityId) || baseObjectFormId == 0 || count == 0) return;
            std::scoped_lock lock(g_stateMutex);
            for (const auto& [requestId, pending] : g_pendingDynamicRemovals) {
                (void)requestId;
                if (pending.entityId == entityId) return;
            }
            PendingDynamicRemoval request;
            request.requestId = NewTransactionId();
            request.entityId = entityId;
            request.baseObjectFormId = baseObjectFormId;
            request.count = count;
            g_pendingDynamicRemovals.emplace(request.requestId, request);
        }

        void ApplyDynamicDrop(DynamicDropSnapshot drop)
        {
            if (drop.rollbackPickup && drop.rollbackCount != 0) {
                auto* baseForm = RE::TESForm::LookupByID(drop.baseObjectFormId);
                auto* baseObject = baseForm ? baseForm->As<RE::TESBoundObject>() : nullptr;
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (baseObject && player) {
                    g_applyingAuthoritative.store(true, std::memory_order_release);
                    player->RemoveItem(baseObject, drop.rollbackCount, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    g_applyingAuthoritative.store(false, std::memory_order_release);
                    logs::warn("[WORLD-DROP-PICKUP-ROLLBACK] worldEntity={:016X} base={:08X} count={}",
                        drop.entityId, drop.baseObjectFormId, drop.rollbackCount);
                }
            }

            if (drop.removed) {
                if (auto* reference = NativeEntityRegistry::Resolve(drop.entityId)) {
                    g_applyingAuthoritative.store(true, std::memory_order_release);
                    if (!reference->IsDisabled()) reference->Disable();
                    g_applyingAuthoritative.store(false, std::memory_order_release);
                }
                NativeEntityRegistry::Unbind(drop.entityId);
                logs::info("[WORLD-DROP-REMOVE] worldEntity={:016X} revision={}", drop.entityId, drop.revision);
                return;
            }

            auto originatingReference = drop.originatingHandle.get();
            RE::TESObjectREFR* reference = originatingReference.get();
            if (!reference) reference = NativeEntityRegistry::Resolve(drop.entityId);
            if (!reference) {
                auto* baseForm = RE::TESForm::LookupByID(drop.baseObjectFormId);
                auto* baseObject = baseForm ? baseForm->As<RE::TESBoundObject>() : nullptr;
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(drop.cellFormId);
                auto* world = drop.worldspaceFormId != 0 ? RE::TESForm::LookupByID<RE::TESWorldSpace>(drop.worldspaceFormId) : nullptr;
                auto* handler = RE::TESDataHandler::GetSingleton();
                if (!baseObject || !cell || !handler) {
                    logs::warn("[WORLD-DROP] unresolved native data worldEntity={:016X} base={:08X} cell={:08X}",
                        drop.entityId, drop.baseObjectFormId, drop.cellFormId);
                    return;
                }
                const auto handle = handler->CreateReferenceAtLocation(
                    baseObject, drop.position, drop.rotation, cell, world, nullptr, nullptr,
                    RE::ObjectRefHandle(), false, true);
                reference = handle.get().get();
                if (!reference) {
                    logs::warn("[WORLD-DROP] native creation failed worldEntity={:016X}", drop.entityId);
                    return;
                }
            }

            reference->SetTemporary();
            reference->extraList.SetCount(drop.count);
            reference->SetPosition(drop.position);
            reference->data.angle = drop.rotation;
            reference->Update3DPosition(true);
            if (!NativeEntityRegistry::BindRuntime(drop.entityId, *reference, NativeEntityKind::DynamicReference)) return;
            logs::info("[WORLD-DROP-SPAWN] worldEntity={:016X} form={:08X} base={:08X} count={} revision={}",
                drop.entityId, reference->GetFormID(), drop.baseObjectFormId, drop.count, drop.revision);
        }

        void ScheduleDynamicDrop(DynamicDropSnapshot drop)
        {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([drop = std::move(drop)]() mutable { ApplyDynamicDrop(std::move(drop)); });
            }
        }

        bool IsPlayer(const RE::TESObjectREFR* reference)
        {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            return reference && player && reference->GetFormID() == player->GetFormID();
        }

        void RememberActivation(RE::TESObjectREFR* reference)
        {
            if (!reference || reference->As<RE::Actor>()) return;

            const auto formId = reference->GetFormID();
            CanonicalKey key;
            const auto entityId = NativeEntityRegistry::FindByRuntimeFormId(formId);
            if (entityId == 0 && !RuntimeFormToCanonical(formId, key)) return;

            const auto* baseObject = reference->GetBaseObject();
            if (!baseObject) return;

            {
                std::scoped_lock lock(g_stateMutex);
                g_recentActivation = RecentActivation{
                    formId,
                    baseObject->GetFormID(),
                    entityId,
                    std::chrono::steady_clock::now()
                };
            }

            logs::info(
                "[WORLD-STATE-PICKUP] captured activation ref={:08X} base={:08X}",
                formId,
                baseObject->GetFormID());
        }

        std::uint32_t ResolveWorldPickupReference(const RE::TESContainerChangedEvent* event)
        {
            if (!event || event->oldContainer != 0) return 0;

            if (auto reference = event->reference.get(); reference && !reference->As<RE::Actor>()) {
                const auto formId = reference->GetFormID();
                CanonicalKey key;
                if (RuntimeFormToCanonical(formId, key)) {
                    std::scoped_lock lock(g_stateMutex);
                    g_recentActivation.reset();
                    return formId;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            std::scoped_lock lock(g_stateMutex);
            if (!g_recentActivation) return 0;

            if (now - g_recentActivation->capturedAt > kPickupCorrelationWindow) {
                g_recentActivation.reset();
                return 0;
            }

            if (event->baseObj == 0 || g_recentActivation->baseObjectFormId != event->baseObj) {
                return 0;
            }

            const auto formId = g_recentActivation->referenceFormId;
            g_recentActivation.reset();
            return formId;
        }

        WorldEntityId ResolveDynamicPickupEntity(const RE::TESContainerChangedEvent* event)
        {
            if (!event || event->oldContainer != 0) return 0;
            if (auto reference = event->reference.get()) {
                const auto id = NativeEntityRegistry::FindByRuntimeFormId(reference->GetFormID());
                if (IsDynamicWorldEntity(id)) return id;
            }
            const auto now = std::chrono::steady_clock::now();
            std::scoped_lock lock(g_stateMutex);
            if (!g_recentActivation || !IsDynamicWorldEntity(g_recentActivation->worldEntityId) ||
                now - g_recentActivation->capturedAt > kPickupCorrelationWindow ||
                event->baseObj == 0 || event->baseObj != g_recentActivation->baseObjectFormId) return 0;
            const auto id = g_recentActivation->worldEntityId;
            g_recentActivation.reset();
            return id;
        }

        class WorldEventSink final :
            public RE::BSTEventSink<RE::TESOpenCloseEvent>,
            public RE::BSTEventSink<RE::TESActivateEvent>,
            public RE::BSTEventSink<RE::TESContainerChangedEvent>,
            public RE::BSTEventSink<RE::TESObjectLoadedEvent>
        {
        public:
            static WorldEventSink* GetSingleton()
            {
                static WorldEventSink sink;
                return std::addressof(sink);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESOpenCloseEvent* event,
                RE::BSTEventSource<RE::TESOpenCloseEvent>*) override
            {
                if (!event || !event->ref || !IsPlayer(event->activeRef.get()) ||
                    g_applyingAuthoritative.load(std::memory_order_acquire)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                ReferenceState state;
                state.enabled = true;
                state.open = event->opened;
                state.hasOpenState = true;
                QueueObservation(event->ref->GetFormID(), state);
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESActivateEvent* event,
                RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                if (!event || !event->objectActivated || !IsPlayer(event->actionRef.get()) ||
                    g_applyingAuthoritative.load(std::memory_order_acquire)) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* activated = event->objectActivated.get();
                if (!activated || activated->As<RE::Actor>()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                RememberActivation(activated);

                const auto formId = activated->GetFormID();
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([formId]() {
                        auto* form = RE::TESForm::LookupByID(formId);
                        auto* reference = form ? form->As<RE::TESObjectREFR>() : nullptr;
                        if (!reference || reference->As<RE::Actor>()) return;
                        if (reference->IsDisabled()) {
                            ReferenceState state;
                            state.enabled = false;
                            QueueObservation(formId, state);
                        }
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESContainerChangedEvent* event,
                RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
            {
                const auto* player = RE::PlayerCharacter::GetSingleton();
                if (event && player &&
                    (event->oldContainer == player->GetFormID() || event->newContainer == player->GetFormID())) {
                    const auto reference = event->reference.get();
                    logs::info("[WORLD-CONTAINER-EVENT] old={:08X} new={:08X} base={:08X} count={} ref={:08X} unique={} applying={}",
                        event->oldContainer,
                        event->newContainer,
                        event->baseObj,
                        event->itemCount,
                        reference ? reference->GetFormID() : 0,
                        event->uniqueID,
                        g_applyingAuthoritative.load(std::memory_order_acquire));
                }
                if (!event || !player || event->newContainer != player->GetFormID() ||
                    g_applyingAuthoritative.load(std::memory_order_acquire)) {
                    if (event && player && event->oldContainer == player->GetFormID() && event->newContainer == 0 &&
                        !g_applyingAuthoritative.load(std::memory_order_acquire)) {
                        QueueDroppedItem(event, *player);
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto dynamicEntityId = ResolveDynamicPickupEntity(event);
                if (dynamicEntityId != 0 && event->itemCount > 0 && event->itemCount <= 32767) {
                    QueueDynamicRemoval(
                        dynamicEntityId,
                        event->baseObj,
                        static_cast<std::uint16_t>(event->itemCount));
                    logs::info("[WORLD-DROP-PICKUP] worldEntity={:016X} base={:08X} count={}",
                        dynamicEntityId, event->baseObj, event->itemCount);
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto formId = ResolveWorldPickupReference(event);
                if (formId == 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                ReferenceState state;
                state.enabled = false;
                state.removed = true;
                QueueObservation(formId, state);

                logs::info(
                    "[WORLD-STATE-PICKUP] correlated player pickup ref={:08X} base={:08X} count={}",
                    formId,
                    event->baseObj,
                    event->itemCount);
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESObjectLoadedEvent* event,
                RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
            {
                if (!event || event->formID == 0) return RE::BSEventNotifyControl::kContinue;
                if (event->loaded) {
                    ResolveDropIntentFromLoadedReference(event->formID);
                    const auto entityId = NativeEntityRegistry::FindByRuntimeFormId(event->formID);
                    if (entityId != 0) ScheduleApply(entityId);
                } else if (!g_applyingAuthoritative.load(std::memory_order_acquire)) {
                    const auto formId = event->formID;
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([formId]() {
                            auto* form = RE::TESForm::LookupByID(formId);
                            auto* reference = form ? form->As<RE::TESObjectREFR>() : nullptr;
                            if (!reference || reference->As<RE::Actor>()) return;
                            if (reference->IsDisabled()) {
                                ReferenceState state;
                                state.enabled = false;
                                QueueObservation(formId, state);
                            }
                        });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        bool InstallEventSink()
        {
            if (g_sinkInstalled.load(std::memory_order_acquire)) return true;
            auto* source = RE::ScriptEventSourceHolder::GetSingleton();
            if (!source) return false;
            auto* sink = WorldEventSink::GetSingleton();
            source->AddEventSink<RE::TESOpenCloseEvent>(sink);
            source->AddEventSink<RE::TESActivateEvent>(sink);
            source->AddEventSink<RE::TESContainerChangedEvent>(sink);
            source->AddEventSink<RE::TESObjectLoadedEvent>(sink);
            g_sinkInstalled.store(true, std::memory_order_release);
            logs::info("[WORLD-STATE] door/activation/container/load event sinks installed");
            return true;
        }

        void Worker(std::stop_token stop)
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                logs::error("[WORLD-STATE] WSAStartup failed");
                return;
            }

            SOCKET socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socketValue == INVALID_SOCKET) {
                WSACleanup();
                logs::error("[WORLD-STATE] UDP socket creation failed");
                return;
            }

            u_long nonBlocking = 1;
            ioctlsocket(socketValue, FIONBIO, &nonBlocking);

            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = 0;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(socketValue, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
                logs::error("[WORLD-STATE] UDP bind failed error={}", WSAGetLastError());
                closesocket(socketValue);
                WSACleanup();
                return;
            }

            const auto baseTarget = ReadServerTarget();
            if (baseTarget.port == 65535) {
                logs::error("[WORLD-STATE] base server port 65535 leaves no port for world-state channel");
                closesocket(socketValue);
                WSACleanup();
                return;
            }

            sockaddr_in server{};
            server.sin_family = AF_INET;
            server.sin_port = htons(static_cast<std::uint16_t>(baseTarget.port + 1));
            server.sin_addr.s_addr = inet_addr(baseTarget.address.c_str());
            if (server.sin_addr.s_addr == INADDR_NONE) {
                logs::error("[WORLD-STATE] invalid server IPv4={}", baseTarget.address);
                closesocket(socketValue);
                WSACleanup();
                return;
            }

            bool welcomed = false;
            auto lastHello = std::chrono::steady_clock::time_point{};
            auto lastServerPacket = std::chrono::steady_clock::time_point{};

            logs::info(
                "[WORLD-STATE] worker started server={}:{} protocol={} authority=server persistence=true",
                baseTarget.address,
                static_cast<unsigned>(baseTarget.port + 1),
                kWorldProtocol);

            while (!stop.stop_requested() && g_running.load(std::memory_order_relaxed)) {
                const auto now = std::chrono::steady_clock::now();
                std::vector<std::uint64_t> dropIntentProbes;
                {
                    std::scoped_lock lock(g_stateMutex);
                    for (auto& [requestId, intent] : g_pendingDropIntents) {
                        if (intent.lastProbeScheduled.time_since_epoch().count() == 0 ||
                            now - intent.lastProbeScheduled >= 50ms) {
                            dropIntentProbes.push_back(requestId);
                            intent.lastProbeScheduled = now;
                        }
                    }
                }
                if (!dropIntentProbes.empty()) {
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        for (const auto requestId : dropIntentProbes) {
                            tasks->AddTask([requestId]() { ResolveDropIntentNearPlayer(requestId); });
                        }
                    }
                }

                const auto helloInterval = welcomed ? 5s : 1s;
                if (lastHello.time_since_epoch().count() == 0 || now - lastHello >= helloInterval) {
                    SendPacket(socketValue, server, MakeHello());
                    lastHello = now;
                }
                if (welcomed && lastServerPacket.time_since_epoch().count() != 0 && now - lastServerPacket >= 10s) {
                    welcomed = false;
                    logs::warn("[WORLD-STATE] server timeout; re-registering");
                }

                if (welcomed) {
                    std::vector<std::pair<std::uint32_t, ReferenceState>> observations;
                    std::vector<PendingDropRequest> dropRequests;
                    std::vector<PendingDynamicRemoval> dynamicRemovals;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        for (auto& [formId, pending] : g_pending) {
                            if (pending.lastSent.time_since_epoch().count() == 0 || now - pending.lastSent >= 250ms) {
                                observations.emplace_back(formId, pending.state);
                                pending.lastSent = now;
                            }
                        }
                        for (auto& [requestId, request] : g_pendingDrops) {
                            (void)requestId;
                            if (request.lastSent.time_since_epoch().count() == 0 || now - request.lastSent >= 250ms) {
                                dropRequests.push_back(request);
                                request.lastSent = now;
                            }
                        }
                        for (auto& [requestId, request] : g_pendingDynamicRemovals) {
                            (void)requestId;
                            if (request.lastSent.time_since_epoch().count() == 0 || now - request.lastSent >= 250ms) {
                                dynamicRemovals.push_back(request);
                                request.lastSent = now;
                            }
                        }
                    }
                    for (const auto& [formId, state] : observations) {
                        SendPacket(socketValue, server, MakeObservation(formId, state));
                    }
                    for (const auto& request : dropRequests) {
                        SendPacket(socketValue, server, MakeDropRequest(request));
                    }
                    for (const auto& request : dynamicRemovals) {
                        SendPacket(socketValue, server, MakeDynamicRemoval(request));
                    }
                }

                for (;;) {
                    std::vector<std::uint8_t> bytes(kMaxDatagram);
                    sockaddr_in peer{};
                    int peerLength = sizeof(peer);
                    const auto count = recvfrom(
                        socketValue,
                        reinterpret_cast<char*>(bytes.data()),
                        static_cast<int>(bytes.size()),
                        0,
                        reinterpret_cast<sockaddr*>(&peer),
                        &peerLength);
                    if (count == SOCKET_ERROR) {
                        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                        logs::warn("[WORLD-STATE] receive failed error={}", WSAGetLastError());
                        break;
                    }
                    if (count <= 0) break;
                    bytes.resize(static_cast<std::size_t>(count));
                    lastServerPacket = std::chrono::steady_clock::now();

                    try {
                        std::size_t offset = 0;
                        if (Read<std::uint32_t>(bytes, offset) != kWorldMagic ||
                            Read<std::uint16_t>(bytes, offset) != kWorldProtocol) {
                            throw std::runtime_error("world-state protocol mismatch");
                        }
                        const auto kind = static_cast<WorldPacketKind>(Read<std::uint8_t>(bytes, offset));
                        (void)Read<std::uint8_t>(bytes, offset);

                        if (kind == WorldPacketKind::Welcome) {
                            if (offset != bytes.size()) throw std::runtime_error("world-state welcome trailing bytes");
                            if (!welcomed) logs::info("[WORLD-STATE] authoritative world channel connected");
                            welcomed = true;
                            continue;
                        }
                        if (kind == WorldPacketKind::Reject) {
                            const auto reason = Read<std::uint8_t>(bytes, offset);
                            welcomed = false;
                            logs::error("[WORLD-STATE] server rejected world channel reason={}", reason);
                            continue;
                        }
                        if (kind == WorldPacketKind::DropSnapshot) {
                            DynamicDropSnapshot drop;
                            drop.requestId = Read<std::uint64_t>(bytes, offset);
                            drop.entityId = Read<WorldEntityId>(bytes, offset);
                            const auto baseObject = ReadKey(bytes, offset);
                            const auto flags = Read<std::uint8_t>(bytes, offset);
                            const auto cell = ReadKey(bytes, offset);
                            const auto worldspace = ReadKey(bytes, offset);
                            const auto readFloat = [&]() { return std::bit_cast<float>(Read<std::uint32_t>(bytes, offset)); };
                            drop.position = { readFloat(), readFloat(), readFloat() };
                            drop.rotation = { readFloat(), readFloat(), readFloat() };
                            drop.count = Read<std::uint16_t>(bytes, offset);
                            drop.revision = Read<std::uint64_t>(bytes, offset);
                            drop.exterior = (flags & 0x01) != 0;
                            const bool hasCell = (flags & 0x02) != 0;
                            const bool hasWorld = (flags & 0x04) != 0;
                            drop.removed = (flags & 0x08) != 0;
                            drop.pickupAccepted = (flags & 0x10) != 0;
                            drop.baseObjectFormId = CanonicalToRuntimeForm(baseObject);
                            drop.cellFormId = CanonicalToRuntimeForm(cell);
                            drop.worldspaceFormId = hasWorld ? CanonicalToRuntimeForm(worldspace) : 0;
                            if (offset != bytes.size() || !IsDynamicWorldEntity(drop.entityId) ||
                                (flags & ~0x1Fu) != 0 || !hasCell || (drop.exterior && !hasWorld) ||
                                (drop.pickupAccepted && (!drop.removed || drop.requestId == 0)) ||
                                drop.baseObjectFormId == 0 || drop.cellFormId == 0 ||
                                (hasWorld && drop.worldspaceFormId == 0) || drop.count == 0 || drop.revision == 0) {
                                throw std::runtime_error("dynamic drop snapshot invalid");
                            }
                            {
                                std::scoped_lock lock(g_stateMutex);
                                if (drop.requestId != 0) {
                                    const auto pendingDrop = g_pendingDrops.find(drop.requestId);
                                    if (pendingDrop != g_pendingDrops.end()) {
                                        drop.originatingHandle = pendingDrop->second.localHandle;
                                        g_pendingDrops.erase(pendingDrop);
                                    } else {
                                        const auto pendingPickup = g_pendingDynamicRemovals.find(drop.requestId);
                                        if (pendingPickup != g_pendingDynamicRemovals.end()) {
                                            if (pendingPickup->second.entityId != drop.entityId ||
                                                pendingPickup->second.baseObjectFormId != drop.baseObjectFormId ||
                                                pendingPickup->second.count != drop.count) {
                                                throw std::runtime_error("dynamic pickup response does not match pending transaction");
                                            }
                                            if (!drop.pickupAccepted) {
                                                drop.rollbackPickup = true;
                                                drop.rollbackCount = pendingPickup->second.count;
                                            }
                                            g_pendingDynamicRemovals.erase(pendingPickup);
                                        }
                                    }
                                }
                            }
                            ScheduleDynamicDrop(std::move(drop));
                            continue;
                        }
                        if (kind != WorldPacketKind::Snapshot) {
                            throw std::runtime_error("unexpected world-state packet kind");
                        }

                        const auto entityId = Read<WorldEntityId>(bytes, offset);
                        const auto key = ReadKey(bytes, offset);
                        const auto revision = Read<std::uint64_t>(bytes, offset);
                        const auto state = DecodeState(Read<std::uint8_t>(bytes, offset));
                        if (offset != bytes.size() || revision == 0) throw std::runtime_error("world-state snapshot invalid");
                        const auto runtimeFormId = CanonicalToRuntimeForm(key);
                        if (runtimeFormId == 0 || entityId != StaticWorldEntityId(key) ||
                            !NativeEntityRegistry::BindStatic(entityId, runtimeFormId)) continue;

                        bool apply = false;
                        {
                            std::scoped_lock lock(g_stateMutex);
                            const auto existing = g_authoritative.find(entityId);
                            if (existing == g_authoritative.end() || revision > existing->second.revision) {
                                g_authoritative[entityId] = AuthoritativeState{ state, revision, runtimeFormId };
                                apply = true;
                            }
                            const auto pending = g_pending.find(runtimeFormId);
                            if (pending != g_pending.end() && SameState(pending->second.state, state)) {
                                g_pending.erase(pending);
                            }
                        }
                        if (apply) ScheduleApply(entityId);
                    } catch (const std::exception& error) {
                        logs::warn("[WORLD-STATE] discarded packet reason={}", error.what());
                    }
                }

                std::this_thread::sleep_for(10ms);
            }

            closesocket(socketValue);
            WSACleanup();
            logs::info("[WORLD-STATE] worker stopped");
        }
    }

    void WorldStateClient::Start()
    {
        Stop();
        if (!InstallEventSink()) {
            logs::error("[WORLD-STATE] event sink installation failed");
        }
        {
            std::scoped_lock lock(g_stateMutex);
            g_pending.clear();
            g_authoritative.clear();
            g_recentActivation.reset();
            g_pendingDrops.clear();
            g_pendingDropIntents.clear();
            g_pendingDynamicRemovals.clear();
            NativeEntityRegistry::ResetStatic();
        }
        g_running.store(true, std::memory_order_release);
        g_thread = std::jthread(Worker);
    }

    void WorldStateClient::Stop()
    {
        g_running.store(false, std::memory_order_release);
        if (g_thread.joinable()) {
            g_thread.request_stop();
            g_thread.join();
        }
        {
            std::scoped_lock lock(g_stateMutex);
            g_pending.clear();
            g_authoritative.clear();
            g_recentActivation.reset();
            g_pendingDrops.clear();
            g_pendingDropIntents.clear();
            g_pendingDynamicRemovals.clear();
            NativeEntityRegistry::ResetStatic();
        }
    }
}
