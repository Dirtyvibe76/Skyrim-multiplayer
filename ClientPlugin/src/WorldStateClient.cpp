#include "pch.h"

#include "WorldStateClient.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
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
        constexpr std::uint16_t kWorldProtocol = 1;
        constexpr auto kLoadOrderRevision = "7dc35a831945468b790a6b3398236c0fe9fe7c8b32425be9ef07ca1434d6c808";
        constexpr std::size_t kMaxDatagram = 1200;
        constexpr auto kPickupCorrelationWindow = 5s;

        enum class WorldPacketKind : std::uint8_t
        {
            Hello = 1,
            Welcome = 2,
            Observation = 3,
            Snapshot = 4,
            Reject = 5
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
        };

        struct RecentActivation
        {
            std::uint32_t referenceFormId{};
            std::uint32_t baseObjectFormId{};
            std::chrono::steady_clock::time_point capturedAt{};
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
        std::unordered_map<std::uint32_t, AuthoritativeState> g_authoritative;
        std::optional<RecentActivation> g_recentActivation;

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
            AppendKey(payload, key);
            Append(payload, EncodeState(state));
            return MakePacket(WorldPacketKind::Observation, payload);
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

        void ApplyReferenceState(std::uint32_t formId, ReferenceState state, std::uint64_t revision)
        {
            auto* form = RE::TESForm::LookupByID(formId);
            auto* reference = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!reference || reference->As<RE::Actor>()) return;

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
                "[WORLD-STATE-APPLY] form={:08X} revision={} enabled={} openKnown={} open={} removed={}",
                formId,
                revision,
                state.enabled,
                state.hasOpenState,
                state.open,
                state.removed);
        }

        void ScheduleApply(std::uint32_t formId)
        {
            AuthoritativeState authoritative;
            {
                std::scoped_lock lock(g_stateMutex);
                const auto it = g_authoritative.find(formId);
                if (it == g_authoritative.end()) return;
                authoritative = it->second;
            }
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([formId, authoritative]() {
                    ApplyReferenceState(formId, authoritative.state, authoritative.revision);
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
            if (!RuntimeFormToCanonical(formId, key)) return;

            const auto* baseObject = reference->GetBaseObject();
            if (!baseObject) return;

            {
                std::scoped_lock lock(g_stateMutex);
                g_recentActivation = RecentActivation{
                    formId,
                    baseObject->GetFormID(),
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
                if (!event || !player || event->newContainer != player->GetFormID() ||
                    g_applyingAuthoritative.load(std::memory_order_acquire)) {
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
                    ScheduleApply(event->formID);
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
                    {
                        std::scoped_lock lock(g_stateMutex);
                        for (auto& [formId, pending] : g_pending) {
                            if (pending.lastSent.time_since_epoch().count() == 0 || now - pending.lastSent >= 250ms) {
                                observations.emplace_back(formId, pending.state);
                                pending.lastSent = now;
                            }
                        }
                    }
                    for (const auto& [formId, state] : observations) {
                        SendPacket(socketValue, server, MakeObservation(formId, state));
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
                        if (kind != WorldPacketKind::Snapshot) {
                            throw std::runtime_error("unexpected world-state packet kind");
                        }

                        const auto key = ReadKey(bytes, offset);
                        const auto revision = Read<std::uint64_t>(bytes, offset);
                        const auto state = DecodeState(Read<std::uint8_t>(bytes, offset));
                        if (offset != bytes.size() || revision == 0) throw std::runtime_error("world-state snapshot invalid");
                        const auto runtimeFormId = CanonicalToRuntimeForm(key);
                        if (runtimeFormId == 0) continue;

                        bool apply = false;
                        {
                            std::scoped_lock lock(g_stateMutex);
                            const auto existing = g_authoritative.find(runtimeFormId);
                            if (existing == g_authoritative.end() || revision > existing->second.revision) {
                                g_authoritative[runtimeFormId] = AuthoritativeState{ state, revision };
                                apply = true;
                            }
                            const auto pending = g_pending.find(runtimeFormId);
                            if (pending != g_pending.end() && SameState(pending->second.state, state)) {
                                g_pending.erase(pending);
                            }
                        }
                        if (apply) ScheduleApply(runtimeFormId);
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
        }
    }
}
