#include <winsock2.h>
#include <ws2tcpip.h>

#include "WorldStateServer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr std::uint32_t kWorldMagic = 0x31535753u; // "SWS1"
        constexpr std::uint16_t kWorldProtocol = 3;
        constexpr std::size_t kMaxDatagram = 1200;

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

        struct ReferenceState
        {
            bool enabled{ true };
            bool open{};
            bool removed{};
            bool hasOpenState{};
        };

        struct PersistedReferenceState
        {
            ReferenceState state;
            std::uint64_t revision{};
        };

        struct DynamicDropState
        {
            WorldEntityId id{};
            std::uint64_t originRequestId{};
            std::uint64_t acceptedPickupRequestId{};
            CanonicalRecordKey baseObject;
            WorldTransform transform;
            RuntimeEntityLocation location;
            std::uint16_t count{ 1 };
            std::uint64_t revision{ 1 };
            bool removed{};
        };

        struct ClientPeer
        {
            sockaddr_in address{};
            std::chrono::steady_clock::time_point lastSeen{};
        };

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

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalRecordKey& key)
        {
            Append(out, static_cast<std::uint8_t>(key.kind == FormNamespaceKind::Light ? 1u : 0u));
            Append(out, key.namespaceIndex);
            Append(out, key.localId);
        }

        CanonicalRecordKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            CanonicalRecordKey key;
            const auto kind = Read<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("world-state canonical key invalid");
            key.kind = kind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
            key.namespaceIndex = Read<std::uint32_t>(bytes, offset);
            key.localId = Read<std::uint32_t>(bytes, offset);
            return key;
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

        std::uint64_t PeerKey(const sockaddr_in& peer)
        {
            return (static_cast<std::uint64_t>(ntohl(peer.sin_addr.s_addr)) << 16) |
                static_cast<std::uint64_t>(ntohs(peer.sin_port));
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

        std::vector<std::uint8_t> MakeSnapshot(
            WorldEntityId entityId,
            const CanonicalRecordKey& source,
            const PersistedReferenceState& state)
        {
            std::vector<std::uint8_t> payload;
            Append(payload, entityId);
            AppendKey(payload, source);
            Append(payload, state.revision);
            Append(payload, EncodeState(state.state));
            return MakePacket(WorldPacketKind::Snapshot, payload);
        }

        std::vector<std::uint8_t> MakeDropSnapshot(
            std::uint64_t requestId,
            const DynamicDropState& drop,
            bool pickupAccepted = false)
        {
            std::vector<std::uint8_t> payload;
            Append(payload, requestId);
            Append(payload, drop.id);
            AppendKey(payload, drop.baseObject);
            std::uint8_t flags = 0;
            if (drop.location.exterior) flags |= 0x01;
            if (drop.location.hasCell) flags |= 0x02;
            if (drop.location.hasWorldspace) flags |= 0x04;
            if (drop.removed) flags |= 0x08;
            if (pickupAccepted) flags |= 0x10;
            Append(payload, flags);
            AppendKey(payload, drop.location.cell);
            AppendKey(payload, drop.location.worldspace);
            const auto appendFloat = [&](float value) { Append(payload, std::bit_cast<std::uint32_t>(value)); };
            appendFloat(drop.transform.x);
            appendFloat(drop.transform.y);
            appendFloat(drop.transform.z);
            appendFloat(drop.transform.pitch);
            appendFloat(drop.transform.yaw);
            appendFloat(drop.transform.roll);
            Append(payload, drop.count);
            Append(payload, drop.revision);
            return MakePacket(WorldPacketKind::DropSnapshot, payload);
        }
    }

    struct WorldStateServer::Impl
    {
        SOCKET socketValue{ INVALID_SOCKET };
        bool winsockStarted{};
        RuntimeEntityRegistry* registry{};
        std::uint16_t boundPort{};
        std::string loadOrderRevision;
        std::filesystem::path statePath;
        std::unordered_map<CanonicalRecordKey, PersistedReferenceState, CanonicalRecordKeyHash> states;
        std::unordered_map<WorldEntityId, DynamicDropState> dynamicDrops;
        std::unordered_map<std::uint64_t, WorldEntityId> dropRequests;
        std::unordered_map<std::uint64_t, ClientPeer> clients;
        WorldStateServerStats stats;

        void Send(const sockaddr_in& peer, const std::vector<std::uint8_t>& bytes)
        {
            if (socketValue == INVALID_SOCKET || bytes.empty()) return;
            const auto sent = sendto(
                socketValue,
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                reinterpret_cast<const sockaddr*>(&peer),
                sizeof(peer));
            if (sent != static_cast<int>(bytes.size()) && WSAGetLastError() != WSAEWOULDBLOCK) {
                std::cerr << "[WORLD-STATE] send failed error=" << WSAGetLastError() << '\n';
            }
        }

        void SendReject(const sockaddr_in& peer, std::uint8_t reason)
        {
            std::vector<std::uint8_t> payload;
            Append(payload, reason);
            Send(peer, MakePacket(WorldPacketKind::Reject, payload));
        }

        bool ValidSource(const CanonicalRecordKey& source) const
        {
            if (!registry) return false;
            const auto sourceIt = registry->sourceToNetwork.find(source);
            if (sourceIt == registry->sourceToNetwork.end()) return false;
            const auto entityIt = registry->entities.find(sourceIt->second);
            return entityIt != registry->entities.end() &&
                entityIt->second.kind == RuntimeEntityKind::StaticReference;
        }

        void SendSnapshot(const sockaddr_in& peer, const CanonicalRecordKey& source, const PersistedReferenceState& state)
        {
            const auto sourceIt = registry->sourceToNetwork.find(source);
            if (sourceIt == registry->sourceToNetwork.end()) return;
            Send(peer, MakeSnapshot(sourceIt->second, source, state));
            ++stats.snapshotsSent;
        }

        void BroadcastSnapshot(const CanonicalRecordKey& source, const PersistedReferenceState& state)
        {
            for (const auto& [key, peer] : clients) {
                (void)key;
                SendSnapshot(peer.address, source, state);
            }
        }

        void SendDropSnapshot(
            const sockaddr_in& peer,
            std::uint64_t requestId,
            const DynamicDropState& drop,
            bool pickupAccepted = false)
        {
            Send(peer, MakeDropSnapshot(requestId, drop, pickupAccepted));
            ++stats.snapshotsSent;
        }

        void BroadcastDropSnapshot(
            const DynamicDropState& drop,
            std::uint64_t requestId = 0,
            std::uint64_t requestPeer = 0,
            bool requestPickupAccepted = false)
        {
            for (const auto& [key, peer] : clients) {
                const bool requester = key == requestPeer;
                SendDropSnapshot(
                    peer.address,
                    requester ? requestId : 0,
                    drop,
                    requester && requestPickupAccepted);
            }
        }

        std::filesystem::path DropStatePath() const
        {
            return statePath.string() + ".drops";
        }

        void SaveDrops()
        {
            if (statePath.empty()) return;
            const auto path = DropStatePath();
            const auto parent = path.parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            const auto temporary = path.string() + ".tmp";
            std::vector<DynamicDropState> ordered;
            ordered.reserve(dynamicDrops.size());
            for (const auto& [id, drop] : dynamicDrops) {
                (void)id;
                ordered.push_back(drop);
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
            {
                std::ofstream output(temporary, std::ios::trunc);
                if (!output) throw std::runtime_error("failed to open dynamic-drop temporary file");
                output << "SKYRIMMP_DYNAMIC_DROPS 3\n" << std::setprecision(std::numeric_limits<float>::max_digits10);
                for (const auto& drop : ordered) {
                    output << drop.id << ' '
                           << (drop.baseObject.kind == FormNamespaceKind::Light ? 1 : 0) << ' '
                           << drop.baseObject.namespaceIndex << ' ' << drop.baseObject.localId << ' '
                           << drop.transform.x << ' ' << drop.transform.y << ' ' << drop.transform.z << ' '
                           << drop.transform.pitch << ' ' << drop.transform.yaw << ' ' << drop.transform.roll << ' '
                           << (drop.location.exterior ? 1 : 0) << ' '
                           << (drop.location.cell.kind == FormNamespaceKind::Light ? 1 : 0) << ' '
                           << drop.location.cell.namespaceIndex << ' ' << drop.location.cell.localId << ' '
                           << (drop.location.hasWorldspace ? 1 : 0) << ' '
                           << (drop.location.worldspace.kind == FormNamespaceKind::Light ? 1 : 0) << ' '
                           << drop.location.worldspace.namespaceIndex << ' ' << drop.location.worldspace.localId << ' '
                           << drop.count << ' ' << drop.revision << ' '
                           << drop.originRequestId << ' ' << drop.acceptedPickupRequestId << ' '
                           << (drop.removed ? 1 : 0) << '\n';
                }
                if (!output) throw std::runtime_error("failed to write dynamic-drop temporary file");
            }
            std::error_code error;
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
            if (error) throw std::runtime_error("failed to commit dynamic-drop state: " + error.message());
            stats.dynamicDropsPersisted = ordered.size();
        }

        std::size_t LoadDrops()
        {
            dynamicDrops.clear();
            dropRequests.clear();
            std::ifstream input(DropStatePath());
            if (!input) return 0;
            std::string magic;
            unsigned version{};
            if (!(input >> magic >> version) || magic != "SKYRIMMP_DYNAMIC_DROPS" ||
                (version != 1 && version != 2 && version != 3)) {
                throw std::runtime_error("persisted dynamic-drop header is malformed");
            }
            std::size_t restored{};
            while (true) {
                DynamicDropState drop;
                unsigned baseKind{}, exterior{}, cellKind{}, hasWorld{}, worldKind{}, count{}, removed{};
                if (!(input >> drop.id)) {
                    if (input.eof()) break;
                    throw std::runtime_error("persisted dynamic-drop state is malformed");
                }
                if (!(input >> baseKind >> drop.baseObject.namespaceIndex >> drop.baseObject.localId
                    >> drop.transform.x >> drop.transform.y >> drop.transform.z
                    >> drop.transform.pitch >> drop.transform.yaw >> drop.transform.roll
                    >> exterior >> cellKind >> drop.location.cell.namespaceIndex >> drop.location.cell.localId
                    >> hasWorld >> worldKind >> drop.location.worldspace.namespaceIndex >> drop.location.worldspace.localId
                    >> count >> drop.revision) || baseKind > 1 || cellKind > 1 || worldKind > 1 ||
                    exterior > 1 || hasWorld > 1 || count == 0 || count > 32767 || drop.revision == 0) {
                    throw std::runtime_error("persisted dynamic-drop record is malformed");
                }
                if (version == 2) {
                    if (!(input >> drop.originRequestId >> removed) || removed > 1) {
                        throw std::runtime_error("persisted dynamic-drop transaction is malformed");
                    }
                    drop.removed = removed != 0;
                } else if (version >= 3) {
                    if (!(input >> drop.originRequestId >> drop.acceptedPickupRequestId >> removed) || removed > 1) {
                        throw std::runtime_error("persisted dynamic-drop transaction is malformed");
                    }
                    drop.removed = removed != 0;
                    if (!drop.removed && drop.acceptedPickupRequestId != 0) {
                        throw std::runtime_error("active dynamic drop has accepted pickup transaction");
                    }
                }
                drop.baseObject.kind = baseKind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
                drop.location.cell.kind = cellKind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
                drop.location.worldspace.kind = worldKind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
                drop.location.exterior = exterior != 0;
                drop.location.hasCell = true;
                drop.location.hasWorldspace = hasWorld != 0;
                drop.count = static_cast<std::uint16_t>(count);
                if (!registry->validItemBaseRecords.contains(drop.baseObject) ||
                    drop.id == (std::numeric_limits<WorldEntityId>::max)()) continue;
                if (!drop.removed &&
                    !RestoreRuntimeEntity(*registry, drop.id, RuntimeEntityKind::StaticReference, drop.transform, drop.location)) continue;
                registry->nextDynamicId = (std::max)(registry->nextDynamicId, drop.id + 1);
                dynamicDrops.emplace(drop.id, drop);
                if (drop.originRequestId != 0) dropRequests.emplace(drop.originRequestId, drop.id);
                if (!drop.removed) ++restored;
            }
            stats.dynamicDropsPersisted = restored;
            return restored;
        }

        void SaveStates()
        {
            if (statePath.empty()) return;
            const auto parent = statePath.parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            const auto temporary = statePath.string() + ".tmp";

            std::vector<std::pair<CanonicalRecordKey, PersistedReferenceState>> ordered(states.begin(), states.end());
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                const auto ak = static_cast<unsigned>(a.first.kind);
                const auto bk = static_cast<unsigned>(b.first.kind);
                if (ak != bk) return ak < bk;
                if (a.first.namespaceIndex != b.first.namespaceIndex) return a.first.namespaceIndex < b.first.namespaceIndex;
                return a.first.localId < b.first.localId;
            });

            {
                std::ofstream output(temporary, std::ios::trunc);
                if (!output) throw std::runtime_error("failed to open world-reference state temporary file");
                output << "SKYRIMMP_WORLD_REFERENCES 1\n";
                for (const auto& [source, state] : ordered) {
                    output << (source.kind == FormNamespaceKind::Light ? 1 : 0) << ' '
                           << source.namespaceIndex << ' ' << source.localId << ' '
                           << state.revision << ' ' << static_cast<unsigned>(EncodeState(state.state)) << '\n';
                }
                if (!output) throw std::runtime_error("failed to write world-reference state temporary file");
            }

            std::error_code error;
            std::filesystem::remove(statePath, error);
            error.clear();
            std::filesystem::rename(temporary, statePath, error);
            if (error) throw std::runtime_error("failed to commit world-reference state: " + error.message());
            stats.persistedStates = states.size();
        }

        std::size_t LoadStates()
        {
            states.clear();
            std::ifstream input(statePath);
            if (!input) return 0;
            std::string magic;
            unsigned version{};
            if (!(input >> magic >> version) || magic != "SKYRIMMP_WORLD_REFERENCES" || version != 1) {
                throw std::runtime_error("persisted world-reference state header is malformed");
            }
            std::size_t restored = 0;
            while (true) {
                unsigned kind{};
                if (!(input >> kind)) {
                    if (input.eof()) break;
                    throw std::runtime_error("persisted world-reference state is malformed");
                }
                CanonicalRecordKey source;
                std::uint64_t revision{};
                unsigned flags{};
                if (!(input >> source.namespaceIndex >> source.localId >> revision >> flags) ||
                    kind > 1 || revision == 0 || flags > 0x0F) {
                    throw std::runtime_error("persisted world-reference record is malformed");
                }
                source.kind = kind ? FormNamespaceKind::Light : FormNamespaceKind::Full;
                if (!ValidSource(source)) continue;
                const auto state = DecodeState(static_cast<std::uint8_t>(flags));
                states[source] = PersistedReferenceState{ state, revision };
                ++restored;
            }
            stats.persistedStates = states.size();
            return restored;
        }
    };

    WorldStateServer::WorldStateServer() : impl_(std::make_unique<Impl>()) {}

    WorldStateServer::~WorldStateServer()
    {
        Stop();
    }

    void WorldStateServer::Start(
        RuntimeEntityRegistry& registry,
        std::uint16_t port,
        std::string loadOrderRevision,
        std::filesystem::path statePath)
    {
        Stop();
        if (port == 0 || loadOrderRevision.empty()) throw std::runtime_error("invalid world-state server configuration");

        impl_->registry = &registry;
        impl_->loadOrderRevision = std::move(loadOrderRevision);
        impl_->statePath = std::move(statePath);

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("world-state WSAStartup failed");
        impl_->winsockStarted = true;

        impl_->socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl_->socketValue == INVALID_SOCKET) {
            Stop();
            throw std::runtime_error("world-state UDP socket creation failed");
        }

        u_long nonBlocking = 1;
        ioctlsocket(impl_->socketValue, FIONBIO, &nonBlocking);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(impl_->socketValue, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            Stop();
            throw std::runtime_error("world-state UDP bind failed error=" + std::to_string(error));
        }
        impl_->boundPort = port;

        const auto restored = impl_->LoadStates();
        const auto restoredDrops = impl_->LoadDrops();
        std::cout << "[WORLD-STATE] protocol=" << kWorldProtocol
                  << " listening=0.0.0.0:" << impl_->boundPort
                  << " restored=" << restored
                  << " restoredDrops=" << restoredDrops
                  << " path=" << impl_->statePath.string() << '\n';
    }

    void WorldStateServer::Poll()
    {
        if (!impl_ || impl_->socketValue == INVALID_SOCKET) return;
        const auto now = std::chrono::steady_clock::now();

        for (;;) {
            std::vector<std::uint8_t> bytes(kMaxDatagram);
            sockaddr_in peer{};
            int peerLength = sizeof(peer);
            const auto count = recvfrom(
                impl_->socketValue,
                reinterpret_cast<char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                &peerLength);
            if (count == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                std::cerr << "[WORLD-STATE] receive failed error=" << WSAGetLastError() << '\n';
                break;
            }
            if (count <= 0) break;
            bytes.resize(static_cast<std::size_t>(count));

            try {
                std::size_t offset = 0;
                if (Read<std::uint32_t>(bytes, offset) != kWorldMagic ||
                    Read<std::uint16_t>(bytes, offset) != kWorldProtocol) {
                    impl_->SendReject(peer, 1);
                    continue;
                }
                const auto kind = static_cast<WorldPacketKind>(Read<std::uint8_t>(bytes, offset));
                (void)Read<std::uint8_t>(bytes, offset);
                const auto peerKey = PeerKey(peer);

                if (kind == WorldPacketKind::Hello) {
                    const auto length = Read<std::uint8_t>(bytes, offset);
                    if (offset + length != bytes.size()) throw std::runtime_error("world-state hello invalid");
                    const std::string revision(
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.end());
                    if (revision != impl_->loadOrderRevision) {
                        impl_->SendReject(peer, 2);
                        continue;
                    }

                    const bool inserted = !impl_->clients.contains(peerKey);
                    impl_->clients[peerKey] = ClientPeer{ peer, now };
                    if (inserted) ++impl_->stats.clientsRegistered;
                    impl_->Send(peer, MakePacket(WorldPacketKind::Welcome));
                    for (const auto& [source, state] : impl_->states) {
                        impl_->SendSnapshot(peer, source, state);
                    }
                    for (const auto& [id, drop] : impl_->dynamicDrops) {
                        (void)id;
                        if (!drop.removed) impl_->SendDropSnapshot(peer, 0, drop);
                    }
                    continue;
                }

                const auto clientIt = impl_->clients.find(peerKey);
                if (clientIt == impl_->clients.end()) {
                    impl_->SendReject(peer, 3);
                    continue;
                }
                clientIt->second.lastSeen = now;

                if (kind == WorldPacketKind::DropRequest) {
                    const auto requestId = Read<std::uint64_t>(bytes, offset);
                    const auto baseObject = ReadKey(bytes, offset);
                    const auto flags = Read<std::uint8_t>(bytes, offset);
                    const auto cell = ReadKey(bytes, offset);
                    const auto worldspace = ReadKey(bytes, offset);
                    const auto readFloat = [&]() { return std::bit_cast<float>(Read<std::uint32_t>(bytes, offset)); };
                    WorldTransform transform{ readFloat(), readFloat(), readFloat(), readFloat(), readFloat(), readFloat() };
                    const auto itemCount = Read<std::uint16_t>(bytes, offset);
                    const bool exterior = (flags & 0x01) != 0;
                    const bool hasCell = (flags & 0x02) != 0;
                    const bool hasWorld = (flags & 0x04) != 0;
                    const bool finite = std::isfinite(transform.x) && std::isfinite(transform.y) &&
                        std::isfinite(transform.z) && std::isfinite(transform.pitch) &&
                        std::isfinite(transform.yaw) && std::isfinite(transform.roll);
                    if (offset != bytes.size() || requestId == 0 || (flags & ~0x07u) != 0 || !hasCell ||
                        (exterior && !hasWorld) || itemCount == 0 || itemCount > 32767 || !finite ||
                        !impl_->registry->validItemBaseRecords.contains(baseObject)) {
                        ++impl_->stats.observationsRejected;
                        continue;
                    }

                    const auto existing = impl_->dropRequests.find(requestId);
                    if (existing != impl_->dropRequests.end()) {
                        const auto drop = impl_->dynamicDrops.find(existing->second);
                        if (drop != impl_->dynamicDrops.end()) {
                            const auto& persisted = drop->second;
                            const bool sameRequest = persisted.baseObject == baseObject &&
                                persisted.location.cell == cell && persisted.location.worldspace == worldspace &&
                                persisted.location.exterior == exterior && persisted.location.hasCell == hasCell &&
                                persisted.location.hasWorldspace == hasWorld && persisted.count == itemCount &&
                                std::bit_cast<std::uint32_t>(persisted.transform.x) == std::bit_cast<std::uint32_t>(transform.x) &&
                                std::bit_cast<std::uint32_t>(persisted.transform.y) == std::bit_cast<std::uint32_t>(transform.y) &&
                                std::bit_cast<std::uint32_t>(persisted.transform.z) == std::bit_cast<std::uint32_t>(transform.z) &&
                                std::bit_cast<std::uint32_t>(persisted.transform.pitch) == std::bit_cast<std::uint32_t>(transform.pitch) &&
                                std::bit_cast<std::uint32_t>(persisted.transform.yaw) == std::bit_cast<std::uint32_t>(transform.yaw) &&
                                std::bit_cast<std::uint32_t>(persisted.transform.roll) == std::bit_cast<std::uint32_t>(transform.roll);
                            if (sameRequest) impl_->SendDropSnapshot(peer, requestId, persisted);
                            else ++impl_->stats.observationsRejected;
                        }
                        continue;
                    }

                    RuntimeEntityLocation location;
                    location.cell = cell;
                    location.worldspace = worldspace;
                    location.exterior = exterior;
                    location.hasCell = true;
                    location.hasWorldspace = hasWorld;
                    const auto entityId = SpawnRuntimeEntity(
                        *impl_->registry, RuntimeEntityKind::StaticReference, transform, location);
                    DynamicDropState drop;
                    drop.id = entityId;
                    drop.originRequestId = requestId;
                    drop.baseObject = baseObject;
                    drop.transform = transform;
                    drop.location = location;
                    drop.count = itemCount;
                    impl_->dynamicDrops.emplace(entityId, drop);
                    impl_->dropRequests.emplace(requestId, entityId);
                    ++impl_->stats.dynamicDropsSpawned;
                    ++impl_->stats.observationsApplied;
                    impl_->SaveDrops();
                    impl_->BroadcastDropSnapshot(drop, requestId, peerKey);
                    std::cout << "[WORLD-DROP-SPAWN] entity=" << entityId << " count=" << itemCount << '\n';
                    continue;
                }

                if (kind == WorldPacketKind::RemoveDynamic) {
                    const auto pickupRequestId = Read<std::uint64_t>(bytes, offset);
                    const auto entityId = Read<WorldEntityId>(bytes, offset);
                    const auto itemCount = Read<std::uint16_t>(bytes, offset);
                    const auto dropIt = impl_->dynamicDrops.find(entityId);
                    if (offset != bytes.size() || pickupRequestId == 0 || itemCount == 0 ||
                        dropIt == impl_->dynamicDrops.end()) {
                        ++impl_->stats.observationsRejected;
                        continue;
                    }
                    if (!dropIt->second.removed) {
                        if (itemCount != dropIt->second.count) {
                            ++impl_->stats.observationsRejected;
                            impl_->SendDropSnapshot(peer, pickupRequestId, dropIt->second, false);
                            continue;
                        }
                        dropIt->second.acceptedPickupRequestId = pickupRequestId;
                        dropIt->second.removed = true;
                        ++dropIt->second.revision;
                        DespawnRuntimeEntity(*impl_->registry, entityId);
                        ++impl_->stats.dynamicDropsRemoved;
                        ++impl_->stats.observationsApplied;
                        impl_->SaveDrops();
                        impl_->BroadcastDropSnapshot(dropIt->second, pickupRequestId, peerKey, true);
                        std::cout << "[WORLD-DROP-REMOVE] entity=" << entityId
                                  << " request=" << pickupRequestId << '\n';
                    } else {
                        const bool accepted = dropIt->second.acceptedPickupRequestId == pickupRequestId;
                        impl_->SendDropSnapshot(peer, pickupRequestId, dropIt->second, accepted);
                    }
                    continue;
                }

                if (kind != WorldPacketKind::Observation) {
                    throw std::runtime_error("unexpected world-state packet kind");
                }

                ++impl_->stats.observationsReceived;
                const auto entityId = Read<WorldEntityId>(bytes, offset);
                const auto source = ReadKey(bytes, offset);
                const auto state = DecodeState(Read<std::uint8_t>(bytes, offset));
                const auto sourceIt = impl_->registry->sourceToNetwork.find(source);
                if (offset != bytes.size() || !impl_->ValidSource(source) ||
                    sourceIt == impl_->registry->sourceToNetwork.end() || sourceIt->second != entityId) {
                    ++impl_->stats.observationsRejected;
                    continue;
                }

                auto stateIt = impl_->states.find(source);
                if (stateIt == impl_->states.end()) {
                    stateIt = impl_->states.emplace(source, PersistedReferenceState{ state, 1 }).first;
                    ++impl_->stats.observationsApplied;
                    impl_->SaveStates();
                    impl_->BroadcastSnapshot(source, stateIt->second);
                } else if (!SameState(stateIt->second.state, state)) {
                    stateIt->second.state = state;
                    ++stateIt->second.revision;
                    ++impl_->stats.observationsApplied;
                    impl_->SaveStates();
                    impl_->BroadcastSnapshot(source, stateIt->second);
                } else {
                    impl_->SendSnapshot(peer, source, stateIt->second);
                }
            } catch (const std::exception& error) {
                ++impl_->stats.observationsRejected;
                std::cerr << "[WORLD-STATE] discarded packet reason=\"" << error.what() << "\"\n";
            }
        }

        for (auto it = impl_->clients.begin(); it != impl_->clients.end();) {
            if (now - it->second.lastSeen >= 15s) it = impl_->clients.erase(it);
            else ++it;
        }
    }

    void WorldStateServer::Stop()
    {
        if (!impl_) return;
        if (impl_->registry && !impl_->statePath.empty()) {
            try {
                impl_->SaveStates();
                impl_->SaveDrops();
            } catch (const std::exception& error) {
                std::cerr << "[WORLD-STATE] final persistence failed reason=\"" << error.what() << "\"\n";
            }
        }
        if (impl_->socketValue != INVALID_SOCKET) {
            closesocket(impl_->socketValue);
            impl_->socketValue = INVALID_SOCKET;
        }
        if (impl_->winsockStarted) {
            WSACleanup();
            impl_->winsockStarted = false;
        }
        impl_->clients.clear();
        impl_->states.clear();
        impl_->dynamicDrops.clear();
        impl_->dropRequests.clear();
        impl_->registry = nullptr;
        impl_->boundPort = 0;
    }

    std::uint16_t WorldStateServer::BoundPort() const noexcept
    {
        return impl_ ? impl_->boundPort : 0;
    }

    std::size_t WorldStateServer::ClientCount() const noexcept
    {
        return impl_ ? impl_->clients.size() : 0;
    }

    const WorldStateServerStats& WorldStateServer::Stats() const noexcept
    {
        return impl_->stats;
    }
}
