#include "WireProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace SkyrimMP::Server
{
    namespace
    {
        template <class T>
        void AppendIntegral(std::vector<std::uint8_t>& out, T value)
        {
            using U = std::make_unsigned_t<T>;
            const U u = static_cast<U>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                out.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFFu));
            }
        }

        void AppendFloat(std::vector<std::uint8_t>& out, float value)
        {
            AppendIntegral(out, std::bit_cast<std::uint32_t>(value));
        }

        template <class T>
        T ReadIntegral(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("wire packet truncated");
            using U = std::make_unsigned_t<T>;
            U value{};
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value |= static_cast<U>(bytes[offset++]) << (i * 8);
            }
            return static_cast<T>(value);
        }

        float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            return std::bit_cast<float>(ReadIntegral<std::uint32_t>(bytes, offset));
        }

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalRecordKey& key)
        {
            AppendIntegral(out, static_cast<std::uint8_t>(key.kind == FormNamespaceKind::Light ? 1u : 0u));
            AppendIntegral(out, key.namespaceIndex);
            AppendIntegral(out, key.localId);
        }

        CanonicalRecordKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            CanonicalRecordKey key;
            const auto kind = ReadIntegral<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("invalid canonical namespace kind on wire");
            key.kind = kind == 1 ? FormNamespaceKind::Light : FormNamespaceKind::Full;
            key.namespaceIndex = ReadIntegral<std::uint32_t>(bytes, offset);
            key.localId = ReadIntegral<std::uint32_t>(bytes, offset);
            return key;
        }

        void AppendSnapshot(std::vector<std::uint8_t>& out, const ReplicatedEntitySnapshot& snapshot)
        {
            AppendIntegral(out, snapshot.id);
            AppendIntegral(out, static_cast<std::uint8_t>(snapshot.kind));
            std::uint8_t flags = 0;
            if (snapshot.hasSourceRecord) flags |= 0x01;
            if (snapshot.location.exterior) flags |= 0x02;
            if (snapshot.location.hasCell) flags |= 0x04;
            if (snapshot.location.hasWorldspace) flags |= 0x08;
            if (snapshot.hasActorState) flags |= 0x10;
            if (snapshot.hasStatusState) flags |= 0x20;
            AppendIntegral(out, flags);
            AppendIntegral(out, static_cast<std::uint16_t>(0));
            AppendIntegral(out, snapshot.revision);
            AppendFloat(out, snapshot.transform.x);
            AppendFloat(out, snapshot.transform.y);
            AppendFloat(out, snapshot.transform.z);
            AppendFloat(out, snapshot.transform.pitch);
            AppendFloat(out, snapshot.transform.yaw);
            AppendFloat(out, snapshot.transform.roll);
            AppendKey(out, snapshot.location.cell);
            AppendKey(out, snapshot.location.worldspace);
            AppendKey(out, snapshot.sourceRecord);
            if (snapshot.kind == RuntimeEntityKind::Player) {
                AppendFloat(out, snapshot.health);
                AppendFloat(out, snapshot.magicka);
                AppendFloat(out, snapshot.stamina);
                std::uint8_t actorFlags = 0;
                if (snapshot.dead) actorFlags |= 0x01;
                if (snapshot.inCombat) actorFlags |= 0x02;
                AppendIntegral(out, actorFlags);
                AppendIntegral(out, snapshot.actionFlags);
                if (snapshot.equippedFormIds.size() > 32) throw std::runtime_error("snapshot equipment exceeds wire limit");
                AppendIntegral(out, static_cast<std::uint8_t>(snapshot.equippedFormIds.size()));
                for (const auto formId : snapshot.equippedFormIds) AppendIntegral(out, formId);
            }
        }

        ReplicatedEntitySnapshot ReadSnapshot(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            ReplicatedEntitySnapshot snapshot;
            snapshot.id = ReadIntegral<NetworkEntityId>(bytes, offset);
            const auto kind = ReadIntegral<std::uint8_t>(bytes, offset);
            if (kind > static_cast<std::uint8_t>(RuntimeEntityKind::Player)) throw std::runtime_error("invalid runtime entity kind on wire");
            snapshot.kind = static_cast<RuntimeEntityKind>(kind);
            const auto flags = ReadIntegral<std::uint8_t>(bytes, offset);
            (void)ReadIntegral<std::uint16_t>(bytes, offset);
            snapshot.hasSourceRecord = (flags & 0x01) != 0;
            snapshot.location.exterior = (flags & 0x02) != 0;
            snapshot.location.hasCell = (flags & 0x04) != 0;
            snapshot.location.hasWorldspace = (flags & 0x08) != 0;
            snapshot.hasActorState = (flags & 0x10) != 0;
            snapshot.hasStatusState = (flags & 0x20) != 0;
            snapshot.revision = ReadIntegral<std::uint64_t>(bytes, offset);
            snapshot.transform.x = ReadFloat(bytes, offset);
            snapshot.transform.y = ReadFloat(bytes, offset);
            snapshot.transform.z = ReadFloat(bytes, offset);
            snapshot.transform.pitch = ReadFloat(bytes, offset);
            snapshot.transform.yaw = ReadFloat(bytes, offset);
            snapshot.transform.roll = ReadFloat(bytes, offset);
            snapshot.location.cell = ReadKey(bytes, offset);
            snapshot.location.worldspace = ReadKey(bytes, offset);
            snapshot.sourceRecord = ReadKey(bytes, offset);
            if (snapshot.kind == RuntimeEntityKind::Player) {
                snapshot.health = ReadFloat(bytes, offset);
                snapshot.magicka = ReadFloat(bytes, offset);
                snapshot.stamina = ReadFloat(bytes, offset);
                const auto actorFlags = ReadIntegral<std::uint8_t>(bytes, offset);
                if ((actorFlags & ~0x03u) != 0) throw std::runtime_error("invalid actor-state flags on wire");
                snapshot.dead = (actorFlags & 0x01) != 0;
                snapshot.inCombat = (actorFlags & 0x02) != 0;
                snapshot.actionFlags = ReadIntegral<std::uint16_t>(bytes, offset);
                if ((snapshot.actionFlags & ~static_cast<std::uint16_t>((1u << 9) - 1)) != 0) {
                    throw std::runtime_error("invalid action-state flags on wire");
                }
                const auto equipmentCount = ReadIntegral<std::uint8_t>(bytes, offset);
                if (equipmentCount > 32) throw std::runtime_error("snapshot equipment exceeds wire limit");
                snapshot.equippedFormIds.reserve(equipmentCount);
                for (std::uint8_t i = 0; i < equipmentCount; ++i) {
                    snapshot.equippedFormIds.push_back(ReadIntegral<std::uint32_t>(bytes, offset));
                }
            }
            return snapshot;
        }

        ReplicationMessage MakeTestMessage(ReplicationMessageKind kind, bool reliable, std::uint64_t revision)
        {
            ReplicationMessage message;
            message.kind = kind;
            message.id = 0x8000000000001234ull;
            message.revision = revision;
            message.reliable = reliable;
            if (kind != ReplicationMessageKind::Despawn) {
                message.snapshot.id = message.id;
                message.snapshot.kind = RuntimeEntityKind::Player;
                message.snapshot.revision = revision;
                message.snapshot.transform = WorldTransform{ 123.25f, -456.5f, 789.75f, 0.1f, 0.2f, 0.3f };
                message.snapshot.location.hasCell = true;
                message.snapshot.location.exterior = true;
                message.snapshot.location.hasWorldspace = true;
                message.snapshot.location.cell = CanonicalRecordKey{ FormNamespaceKind::Full, 0, 0x12345 };
                message.snapshot.location.worldspace = CanonicalRecordKey{ FormNamespaceKind::Full, 0, 0x3C };
                message.snapshot.hasSourceRecord = false;
                message.snapshot.hasActorState = true;
                message.snapshot.hasStatusState = true;
                message.snapshot.health = 87.5f;
                message.snapshot.magicka = 42.0f;
                message.snapshot.stamina = 63.25f;
                message.snapshot.inCombat = true;
                message.snapshot.actionFlags = 0x0095;
                message.snapshot.equippedFormIds = { 0x00012EB7u, 0x0001397Eu };
            }
            return message;
        }

        struct WinsockGuard
        {
            WinsockGuard()
            {
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed for wire self-test");
            }
            ~WinsockGuard() { WSACleanup(); }
        };

        struct SocketGuard
        {
            SOCKET value{ INVALID_SOCKET };
            ~SocketGuard() { if (value != INVALID_SOCKET) closesocket(value); }
        };
    }

    std::vector<std::uint8_t> SerializeWirePacket(const WirePacket& packet)
    {
        if (packet.messages.size() > 0xFFFFu) throw std::runtime_error("too many replication messages for one wire packet");
        if (packet.controlPayload.size() > 0xFFFFu) throw std::runtime_error("wire control payload too large");
        if (packet.kind == WirePacketKind::Control && !packet.messages.empty()) throw std::runtime_error("control wire packet cannot contain replication messages");
        if (packet.kind != WirePacketKind::Control && !packet.controlPayload.empty()) throw std::runtime_error("non-control wire packet cannot contain control payload");
        if (packet.kind == WirePacketKind::Ack && !packet.messages.empty()) throw std::runtime_error("ACK wire packet cannot contain replication messages");

        std::vector<std::uint8_t> out;
        out.reserve(64 + packet.messages.size() * 96 + packet.controlPayload.size());
        AppendIntegral(out, kWireMagic);
        AppendIntegral(out, kWireProtocolVersion);
        AppendIntegral(out, static_cast<std::uint8_t>(packet.kind));
        AppendIntegral(out, static_cast<std::uint8_t>(packet.channel));
        AppendIntegral(out, packet.sequence);
        AppendIntegral(out, packet.ackSequence);
        AppendIntegral(out, static_cast<std::uint16_t>(packet.messages.size()));
        AppendIntegral(out, static_cast<std::uint16_t>(packet.controlPayload.size()));

        for (const auto& message : packet.messages) {
            AppendIntegral(out, static_cast<std::uint8_t>(message.kind));
            AppendIntegral(out, static_cast<std::uint8_t>(message.reliable ? 1u : 0u));
            AppendIntegral(out, static_cast<std::uint16_t>(0));
            AppendIntegral(out, message.id);
            AppendIntegral(out, message.revision);
            if (message.kind != ReplicationMessageKind::Despawn) AppendSnapshot(out, message.snapshot);
        }
        out.insert(out.end(), packet.controlPayload.begin(), packet.controlPayload.end());

        if (out.size() > kMaxUdpDatagramBytes) throw std::runtime_error("wire packet exceeds maximum UDP datagram payload");
        return out;
    }

    WirePacket DeserializeWirePacket(const std::vector<std::uint8_t>& bytes)
    {
        std::size_t offset = 0;
        if (ReadIntegral<std::uint32_t>(bytes, offset) != kWireMagic) throw std::runtime_error("wire packet magic mismatch");
        if (ReadIntegral<std::uint16_t>(bytes, offset) != kWireProtocolVersion) throw std::runtime_error("wire protocol version mismatch");

        WirePacket packet;
        const auto kind = ReadIntegral<std::uint8_t>(bytes, offset);
        const auto channel = ReadIntegral<std::uint8_t>(bytes, offset);
        if (kind < 1 || kind > 3) throw std::runtime_error("invalid wire packet kind");
        if (channel > 1) throw std::runtime_error("invalid wire channel");
        packet.kind = static_cast<WirePacketKind>(kind);
        packet.channel = static_cast<WireChannel>(channel);
        packet.sequence = ReadIntegral<std::uint32_t>(bytes, offset);
        packet.ackSequence = ReadIntegral<std::uint32_t>(bytes, offset);
        const auto messageCount = ReadIntegral<std::uint16_t>(bytes, offset);
        const auto controlSize = ReadIntegral<std::uint16_t>(bytes, offset);

        if (packet.kind == WirePacketKind::Control && messageCount != 0) throw std::runtime_error("control packet has replication messages");
        if (packet.kind != WirePacketKind::Control && controlSize != 0) throw std::runtime_error("non-control packet has control payload");
        if (packet.kind == WirePacketKind::Ack && messageCount != 0) throw std::runtime_error("ACK packet has replication messages");

        packet.messages.reserve(messageCount);
        for (std::uint16_t i = 0; i < messageCount; ++i) {
            ReplicationMessage message;
            const auto messageKind = ReadIntegral<std::uint8_t>(bytes, offset);
            const auto reliable = ReadIntegral<std::uint8_t>(bytes, offset);
            (void)ReadIntegral<std::uint16_t>(bytes, offset);
            if (messageKind > static_cast<std::uint8_t>(ReplicationMessageKind::Despawn)) throw std::runtime_error("invalid replication message kind on wire");
            if (reliable > 1) throw std::runtime_error("invalid reliable flag on wire");
            message.kind = static_cast<ReplicationMessageKind>(messageKind);
            message.reliable = reliable != 0;
            message.id = ReadIntegral<NetworkEntityId>(bytes, offset);
            message.revision = ReadIntegral<std::uint64_t>(bytes, offset);
            if (message.kind != ReplicationMessageKind::Despawn) message.snapshot = ReadSnapshot(bytes, offset);
            packet.messages.push_back(std::move(message));
        }

        if (offset + controlSize > bytes.size()) throw std::runtime_error("wire control payload truncated");
        packet.controlPayload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + controlSize));
        offset += controlSize;

        if (offset != bytes.size()) throw std::runtime_error("wire packet contains trailing bytes");
        return packet;
    }

    void RunWireProtocolSelfTest()
    {
        WirePacket reliable;
        reliable.kind = WirePacketKind::Data;
        reliable.channel = WireChannel::Reliable;
        reliable.sequence = 41;
        reliable.messages.push_back(MakeTestMessage(ReplicationMessageKind::Spawn, true, 1));
        reliable.messages.push_back(MakeTestMessage(ReplicationMessageKind::Despawn, true, 2));

        const auto reliableBytes = SerializeWirePacket(reliable);
        const auto reliableRoundTrip = DeserializeWirePacket(reliableBytes);
        if (reliableRoundTrip.sequence != reliable.sequence || reliableRoundTrip.messages.size() != 2 ||
            reliableRoundTrip.messages[0].kind != ReplicationMessageKind::Spawn ||
            reliableRoundTrip.messages[1].kind != ReplicationMessageKind::Despawn ||
            !reliableRoundTrip.messages[0].snapshot.hasActorState ||
            !reliableRoundTrip.messages[0].snapshot.hasStatusState ||
            reliableRoundTrip.messages[0].snapshot.equippedFormIds.size() != 2 ||
            reliableRoundTrip.messages[0].snapshot.health != 87.5f ||
            reliableRoundTrip.messages[0].snapshot.magicka != 42.0f ||
            reliableRoundTrip.messages[0].snapshot.stamina != 63.25f ||
            !reliableRoundTrip.messages[0].snapshot.inCombat) {
            throw std::runtime_error("wire serialization reliable round-trip self-test failed");
        }

        WirePacket delta;
        delta.kind = WirePacketKind::Data;
        delta.channel = WireChannel::Unreliable;
        delta.sequence = 42;
        delta.messages.push_back(MakeTestMessage(ReplicationMessageKind::Delta, false, 3));
        const auto deltaBytes = SerializeWirePacket(delta);
        const auto deltaRoundTrip = DeserializeWirePacket(deltaBytes);
        if (deltaRoundTrip.channel != WireChannel::Unreliable || deltaRoundTrip.messages.size() != 1 ||
            deltaRoundTrip.messages.front().snapshot.transform.x != 123.25f) {
            throw std::runtime_error("wire serialization delta round-trip self-test failed");
        }

        WirePacket control;
        control.kind = WirePacketKind::Control;
        control.channel = WireChannel::Reliable;
        control.sequence = 43;
        control.controlPayload = { 0x01, 0x02, 0x03, 0x04 };
        const auto controlBytes = SerializeWirePacket(control);
        const auto controlRoundTrip = DeserializeWirePacket(controlBytes);
        if (controlRoundTrip.kind != WirePacketKind::Control || controlRoundTrip.controlPayload != control.controlPayload) {
            throw std::runtime_error("wire serialization control round-trip self-test failed");
        }

        WinsockGuard winsock;
        SocketGuard receiver;
        SocketGuard sender;
        receiver.value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sender.value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (receiver.value == INVALID_SOCKET || sender.value == INVALID_SOCKET) throw std::runtime_error("failed to create UDP sockets for wire self-test");

        sockaddr_in receiveAddress{};
        receiveAddress.sin_family = AF_INET;
        receiveAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        receiveAddress.sin_port = 0;
        if (bind(receiver.value, reinterpret_cast<const sockaddr*>(&receiveAddress), sizeof(receiveAddress)) == SOCKET_ERROR) {
            throw std::runtime_error("failed to bind UDP loopback receiver");
        }

        int addressLength = sizeof(receiveAddress);
        if (getsockname(receiver.value, reinterpret_cast<sockaddr*>(&receiveAddress), &addressLength) == SOCKET_ERROR) {
            throw std::runtime_error("failed to resolve UDP loopback receiver port");
        }

        DWORD timeoutMs = 1000;
        if (setsockopt(receiver.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == SOCKET_ERROR) {
            throw std::runtime_error("failed to configure UDP loopback receive timeout");
        }

        const auto sent = sendto(sender.value, reinterpret_cast<const char*>(reliableBytes.data()), static_cast<int>(reliableBytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&receiveAddress), sizeof(receiveAddress));
        if (sent != static_cast<int>(reliableBytes.size())) throw std::runtime_error("UDP loopback send failed");

        std::vector<std::uint8_t> received(kMaxUdpDatagramBytes);
        sockaddr_in peer{};
        int peerLength = sizeof(peer);
        const auto receivedBytes = recvfrom(receiver.value, reinterpret_cast<char*>(received.data()), static_cast<int>(received.size()), 0,
            reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (receivedBytes <= 0) throw std::runtime_error("UDP loopback receive failed");
        received.resize(static_cast<std::size_t>(receivedBytes));
        const auto receivedPacket = DeserializeWirePacket(received);
        if (receivedPacket.sequence != reliable.sequence || receivedPacket.messages.size() != reliable.messages.size()) {
            throw std::runtime_error("UDP loopback decoded packet mismatch");
        }

        WirePacket ack;
        ack.kind = WirePacketKind::Ack;
        ack.channel = WireChannel::Reliable;
        ack.sequence = 100;
        ack.ackSequence = receivedPacket.sequence;
        const auto ackBytes = SerializeWirePacket(ack);
        const auto ackRoundTrip = DeserializeWirePacket(ackBytes);
        if (ackRoundTrip.kind != WirePacketKind::Ack || ackRoundTrip.ackSequence != reliable.sequence) {
            throw std::runtime_error("wire ACK round-trip self-test failed");
        }

        std::cout << "[WIRE] protocol=" << kWireProtocolVersion
                  << " magic=SMP1 maxDatagram=" << kMaxUdpDatagramBytes
                  << " reliableChannel=true unreliableChannel=true control=true ack=true\n";
        std::cout << "[WIRE-SELFTEST] serialize=true deserialize=true reliable=true delta=true control=true udpLoopback=true ack=true"
                  << " reliableBytes=" << reliableBytes.size()
                  << " deltaBytes=" << deltaBytes.size()
                  << " controlBytes=" << controlBytes.size() << '\n';
    }
}
