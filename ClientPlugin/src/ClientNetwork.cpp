#include "pch.h"

#include "ClientNetwork.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace SkyrimMP
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr std::uint32_t kWireMagic = 0x31504D53u;
        constexpr std::uint16_t kWireProtocolVersion = 2;
        constexpr std::uint16_t kReplicationProtocolVersion = 2;
        constexpr std::uint16_t kServerPort = 10578;
        constexpr auto kLoadOrderRevision = "7dc35a831945468b790a6b3398236c0fe9fe7c8b32425be9ef07ca1434d6c808";
        constexpr std::size_t kMaxDatagram = 1200;

        enum class PacketKind : std::uint8_t { Data = 1, Ack = 2, Control = 3 };
        enum class Channel : std::uint8_t { Unreliable = 0, Reliable = 1 };
        enum class ControlKind : std::uint8_t { Hello = 1, Welcome = 2, Reject = 3, Heartbeat = 4, Disconnect = 5, Interest = 6 };
        enum class ReplicationKind : std::uint8_t { Spawn = 0, Delta = 1, Despawn = 2 };

        struct CanonicalKey
        {
            bool light{};
            std::uint32_t namespaceIndex{};
            std::uint32_t localId{};
        };

        struct ClientReplica
        {
            std::uint8_t entityKind{};
            std::uint64_t revision{};
            Vec3 position{};
            Vec3 rotation{};
            CanonicalKey cell{};
            CanonicalKey world{};
            bool exterior{};
            bool hasCell{};
            bool hasWorld{};
        };

        std::atomic_bool g_running{ false };
        std::atomic_bool g_authenticated{ false };
        std::jthread g_thread;
        std::mutex g_playerMutex;
        PlayerState g_player{};
        bool g_hasPlayer = false;

        template <class T>
        void Append(std::vector<std::uint8_t>& out, T value)
        {
            using U = std::make_unsigned_t<T>;
            const U u = static_cast<U>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFFu));
        }

        void AppendFloat(std::vector<std::uint8_t>& out, float value)
        {
            Append(out, std::bit_cast<std::uint32_t>(value));
        }

        template <class T>
        T Read(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("client wire packet truncated");
            using U = std::make_unsigned_t<T>;
            U value{};
            for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<U>(bytes[offset++]) << (i * 8);
            return static_cast<T>(value);
        }

        float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            return std::bit_cast<float>(Read<std::uint32_t>(bytes, offset));
        }

        CanonicalKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            CanonicalKey key;
            const auto kind = Read<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("client canonical key kind invalid");
            key.light = kind == 1;
            key.namespaceIndex = Read<std::uint32_t>(bytes, offset);
            key.localId = Read<std::uint32_t>(bytes, offset);
            return key;
        }

        bool RuntimeFormToCanonical(std::uint32_t formId, CanonicalKey& out)
        {
            if (formId == 0) return false;
            const auto high = static_cast<std::uint8_t>(formId >> 24);
            if (high == 0xFE || high == 0xFF) return false;
            if (high > 4) return false;
            out.light = false;
            out.namespaceIndex = high;
            out.localId = formId & 0x00FFFFFFu;
            return true;
        }

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalKey& key)
        {
            Append(out, static_cast<std::uint8_t>(key.light ? 1u : 0u));
            Append(out, key.namespaceIndex);
            Append(out, key.localId);
        }

        std::vector<std::uint8_t> MakePacket(PacketKind kind, Channel channel, std::uint32_t sequence, std::uint32_t ackSequence, const std::vector<std::uint8_t>& control)
        {
            if (kind == PacketKind::Control && control.empty()) throw std::runtime_error("client control packet missing payload");
            if (kind != PacketKind::Control && !control.empty()) throw std::runtime_error("client non-control packet has control payload");
            if (control.size() > 0xFFFFu) throw std::runtime_error("client control payload too large");

            std::vector<std::uint8_t> out;
            Append(out, kWireMagic);
            Append(out, kWireProtocolVersion);
            Append(out, static_cast<std::uint8_t>(kind));
            Append(out, static_cast<std::uint8_t>(channel));
            Append(out, sequence);
            Append(out, ackSequence);
            Append(out, static_cast<std::uint16_t>(0));
            Append(out, static_cast<std::uint16_t>(kind == PacketKind::Control ? control.size() : 0));
            if (kind == PacketKind::Control) out.insert(out.end(), control.begin(), control.end());
            if (out.size() > kMaxDatagram) throw std::runtime_error("client packet exceeds max datagram");
            return out;
        }

        std::vector<std::uint8_t> EncodeHello(std::uint64_t nonce)
        {
            std::vector<std::uint8_t> out;
            Append(out, static_cast<std::uint8_t>(ControlKind::Hello));
            Append(out, kReplicationProtocolVersion);
            const std::string revision = kLoadOrderRevision;
            Append(out, static_cast<std::uint8_t>(revision.size()));
            out.insert(out.end(), revision.begin(), revision.end());
            Append(out, nonce);
            return out;
        }

        std::vector<std::uint8_t> EncodeHeartbeat(std::uint64_t sessionId)
        {
            std::vector<std::uint8_t> out;
            Append(out, static_cast<std::uint8_t>(ControlKind::Heartbeat));
            Append(out, sessionId);
            return out;
        }

        std::optional<std::vector<std::uint8_t>> EncodeInterest(std::uint64_t sessionId, const PlayerState& player)
        {
            CanonicalKey cell{};
            if (!RuntimeFormToCanonical(player.cellFormId, cell)) return std::nullopt;

            CanonicalKey world{};
            const bool exterior = player.worldspaceFormId != 0;
            if (exterior && !RuntimeFormToCanonical(player.worldspaceFormId, world)) return std::nullopt;

            std::vector<std::uint8_t> out;
            Append(out, static_cast<std::uint8_t>(ControlKind::Interest));
            Append(out, sessionId);
            std::uint8_t flags = 0x02;
            if (exterior) flags |= 0x01 | 0x04;
            Append(out, flags);
            AppendKey(out, cell);
            AppendKey(out, world);
            AppendFloat(out, player.position.x);
            AppendFloat(out, player.position.y);
            AppendFloat(out, player.position.z);
            AppendFloat(out, player.rotation.x);
            AppendFloat(out, player.rotation.y);
            AppendFloat(out, player.rotation.z);
            Append(out, static_cast<std::int32_t>(1));
            return out;
        }

        void SendDatagram(SOCKET socketValue, const sockaddr_in& server, const std::vector<std::uint8_t>& bytes)
        {
            const auto sent = sendto(socketValue, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<const sockaddr*>(&server), sizeof(server));
            if (sent != static_cast<int>(bytes.size())) throw std::runtime_error("client UDP send failed");
        }

        void DecodeReplicationMessages(
            const std::vector<std::uint8_t>& bytes,
            std::size_t& offset,
            std::uint16_t messageCount,
            std::unordered_map<std::uint64_t, ClientReplica>& replicas,
            std::uint64_t& spawns,
            std::uint64_t& deltas,
            std::uint64_t& despawns)
        {
            for (std::uint16_t i = 0; i < messageCount; ++i) {
                const auto kindRaw = Read<std::uint8_t>(bytes, offset);
                const auto reliable = Read<std::uint8_t>(bytes, offset);
                (void)Read<std::uint16_t>(bytes, offset);
                if (kindRaw > static_cast<std::uint8_t>(ReplicationKind::Despawn) || reliable > 1) {
                    throw std::runtime_error("invalid replication message header");
                }
                const auto kind = static_cast<ReplicationKind>(kindRaw);
                const auto id = Read<std::uint64_t>(bytes, offset);
                const auto revision = Read<std::uint64_t>(bytes, offset);

                if (kind == ReplicationKind::Despawn) {
                    replicas.erase(id);
                    ++despawns;
                    continue;
                }

                const auto snapshotId = Read<std::uint64_t>(bytes, offset);
                const auto entityKind = Read<std::uint8_t>(bytes, offset);
                if (entityKind > 2) throw std::runtime_error("invalid replicated entity kind");
                const auto flags = Read<std::uint8_t>(bytes, offset);
                (void)Read<std::uint16_t>(bytes, offset);
                const auto snapshotRevision = Read<std::uint64_t>(bytes, offset);
                if (snapshotId != id || snapshotRevision != revision) throw std::runtime_error("replication envelope/snapshot mismatch");

                ClientReplica replica;
                replica.entityKind = entityKind;
                replica.revision = revision;
                replica.position = { ReadFloat(bytes, offset), ReadFloat(bytes, offset), ReadFloat(bytes, offset) };
                replica.rotation = { ReadFloat(bytes, offset), ReadFloat(bytes, offset), ReadFloat(bytes, offset) };
                replica.cell = ReadKey(bytes, offset);
                replica.world = ReadKey(bytes, offset);
                (void)ReadKey(bytes, offset); // sourceRecord; retained by server identity layer for now
                replica.exterior = (flags & 0x02) != 0;
                replica.hasCell = (flags & 0x04) != 0;
                replica.hasWorld = (flags & 0x08) != 0;

                const auto existing = replicas.find(id);
                if (existing == replicas.end() || revision >= existing->second.revision) {
                    replicas.insert_or_assign(id, replica);
                }
                if (kind == ReplicationKind::Spawn) ++spawns;
                else ++deltas;
            }
        }

        void NetworkThread(std::stop_token stop)
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                logs::error("[NET-CLIENT] WSAStartup failed");
                return;
            }

            SOCKET socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socketValue == INVALID_SOCKET) {
                WSACleanup();
                logs::error("[NET-CLIENT] UDP socket creation failed");
                return;
            }

            u_long nonBlocking = 1;
            ioctlsocket(socketValue, FIONBIO, &nonBlocking);

            sockaddr_in server{};
            server.sin_family = AF_INET;
            server.sin_port = htons(kServerPort);
            server.sin_addr.s_addr = inet_addr("127.0.0.1");

            const std::uint64_t nonce = (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            std::uint64_t sessionId = 0;
            std::uint32_t nextSequence = 1;
            auto lastHello = std::chrono::steady_clock::time_point{};
            auto lastHeartbeat = std::chrono::steady_clock::time_point{};
            auto lastInterest = std::chrono::steady_clock::time_point{};
            std::uint64_t dataPackets = 0;
            std::uint64_t replicationMessages = 0;
            std::uint64_t spawns = 0;
            std::uint64_t deltas = 0;
            std::uint64_t despawns = 0;
            std::unordered_map<std::uint64_t, ClientReplica> replicas;

            logs::info("[NET-CLIENT] worker started server=127.0.0.1:{} protocol={} revision={}", kServerPort, kReplicationProtocolVersion, kLoadOrderRevision);

            while (!stop.stop_requested() && g_running.load(std::memory_order_relaxed)) {
                const auto now = std::chrono::steady_clock::now();
                if (sessionId == 0 && (lastHello.time_since_epoch().count() == 0 || now - lastHello >= 1s)) {
                    SendDatagram(socketValue, server, MakePacket(PacketKind::Control, Channel::Reliable, nextSequence++, 0, EncodeHello(nonce)));
                    lastHello = now;
                }

                if (sessionId != 0) {
                    if (lastHeartbeat.time_since_epoch().count() == 0 || now - lastHeartbeat >= 2s) {
                        SendDatagram(socketValue, server, MakePacket(PacketKind::Control, Channel::Reliable, nextSequence++, 0, EncodeHeartbeat(sessionId)));
                        lastHeartbeat = now;
                    }
                    if (lastInterest.time_since_epoch().count() == 0 || now - lastInterest >= 250ms) {
                        PlayerState player{};
                        bool hasPlayer = false;
                        {
                            std::scoped_lock lock(g_playerMutex);
                            player = g_player;
                            hasPlayer = g_hasPlayer;
                        }
                        if (hasPlayer) {
                            if (auto payload = EncodeInterest(sessionId, player)) {
                                SendDatagram(socketValue, server, MakePacket(PacketKind::Control, Channel::Reliable, nextSequence++, 0, *payload));
                            }
                        }
                        lastInterest = now;
                    }
                }

                for (;;) {
                    std::vector<std::uint8_t> bytes(kMaxDatagram);
                    sockaddr_in peer{};
                    int peerLength = sizeof(peer);
                    const auto count = recvfrom(socketValue, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                        reinterpret_cast<sockaddr*>(&peer), &peerLength);
                    if (count == SOCKET_ERROR) {
                        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                        logs::warn("[NET-CLIENT] UDP receive error={}", WSAGetLastError());
                        break;
                    }
                    if (count <= 0) break;
                    bytes.resize(static_cast<std::size_t>(count));

                    try {
                        std::size_t offset = 0;
                        if (Read<std::uint32_t>(bytes, offset) != kWireMagic) throw std::runtime_error("magic mismatch");
                        if (Read<std::uint16_t>(bytes, offset) != kWireProtocolVersion) throw std::runtime_error("wire version mismatch");
                        const auto kind = static_cast<PacketKind>(Read<std::uint8_t>(bytes, offset));
                        const auto channel = static_cast<Channel>(Read<std::uint8_t>(bytes, offset));
                        const auto sequence = Read<std::uint32_t>(bytes, offset);
                        (void)Read<std::uint32_t>(bytes, offset);
                        const auto messageCount = Read<std::uint16_t>(bytes, offset);
                        const auto controlSize = Read<std::uint16_t>(bytes, offset);

                        if (channel == Channel::Reliable && kind != PacketKind::Ack) {
                            SendDatagram(socketValue, server, MakePacket(PacketKind::Ack, Channel::Reliable, nextSequence++, sequence, {}));
                        }

                        if (kind == PacketKind::Control) {
                            if (messageCount != 0 || controlSize == 0 || offset + controlSize != bytes.size()) throw std::runtime_error("bad control payload");
                            const auto controlKind = static_cast<ControlKind>(Read<std::uint8_t>(bytes, offset));
                            if (controlKind == ControlKind::Welcome) {
                                const auto protocol = Read<std::uint16_t>(bytes, offset);
                                const auto receivedSession = Read<std::uint64_t>(bytes, offset);
                                const auto receivedNonce = Read<std::uint64_t>(bytes, offset);
                                const auto maxPlayers = Read<std::uint32_t>(bytes, offset);
                                if (protocol != kReplicationProtocolVersion || receivedNonce != nonce) throw std::runtime_error("welcome validation failed");
                                if (sessionId == 0) {
                                    sessionId = receivedSession;
                                    g_authenticated.store(true, std::memory_order_relaxed);
                                    logs::info("[NET-CLIENT] authenticated session={} maxPlayers={}", sessionId, maxPlayers);
                                }
                            } else if (controlKind == ControlKind::Reject) {
                                const auto reason = Read<std::uint8_t>(bytes, offset);
                                logs::error("[NET-CLIENT] server rejected connection reason={}", reason);
                            }
                        } else if (kind == PacketKind::Data) {
                            if (controlSize != 0) throw std::runtime_error("data packet has control payload");
                            DecodeReplicationMessages(bytes, offset, messageCount, replicas, spawns, deltas, despawns);
                            if (offset != bytes.size()) throw std::runtime_error("data packet trailing bytes");
                            ++dataPackets;
                            replicationMessages += messageCount;
                            if ((dataPackets % 25) == 1) {
                                logs::info(
                                    "[NET-CLIENT] replication packets={} messages={} active={} spawn={} delta={} despawn={} latestPacketMessages={}",
                                    dataPackets, replicationMessages, replicas.size(), spawns, deltas, despawns, messageCount);
                            }
                        } else if (kind == PacketKind::Ack) {
                            if (messageCount != 0 || controlSize != 0 || offset != bytes.size()) throw std::runtime_error("bad ACK packet");
                        } else {
                            throw std::runtime_error("invalid packet kind");
                        }
                    } catch (const std::exception& e) {
                        logs::warn("[NET-CLIENT] dropped malformed packet: {}", e.what());
                    }
                }

                std::this_thread::sleep_for(2ms);
            }

            g_authenticated.store(false, std::memory_order_relaxed);
            closesocket(socketValue);
            WSACleanup();
            logs::info(
                "[NET-CLIENT] worker stopped packets={} messages={} active={} spawn={} delta={} despawn={}",
                dataPackets, replicationMessages, replicas.size(), spawns, deltas, despawns);
        }
    }

    void ClientNetwork::Start()
    {
        if (g_running.exchange(true, std::memory_order_acq_rel)) return;
        g_thread = std::jthread(NetworkThread);
    }

    void ClientNetwork::Stop()
    {
        if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
        if (g_thread.joinable()) {
            g_thread.request_stop();
            g_thread.join();
        }
        g_authenticated.store(false, std::memory_order_relaxed);
    }

    void ClientNetwork::SubmitLocalPlayer(const PlayerState& player)
    {
        std::scoped_lock lock(g_playerMutex);
        g_player = player;
        g_hasPlayer = player.formId != 0 && player.cellFormId != 0;
    }

    bool ClientNetwork::IsAuthenticated()
    {
        return g_authenticated.load(std::memory_order_relaxed);
    }
}
