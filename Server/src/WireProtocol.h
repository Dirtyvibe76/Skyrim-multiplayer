#pragma once

#include "ReplicationProtocol.h"

#include <cstdint>
#include <vector>

namespace SkyrimMP::Server
{
    constexpr std::uint32_t kWireMagic = 0x31504D53u; // "SMP1" little-endian
    constexpr std::uint16_t kWireProtocolVersion = 1;
    constexpr std::size_t kMaxUdpDatagramBytes = 1200;

    enum class WirePacketKind : std::uint8_t
    {
        Data = 1,
        Ack = 2
    };

    enum class WireChannel : std::uint8_t
    {
        Unreliable = 0,
        Reliable = 1
    };

    struct WirePacket
    {
        WirePacketKind kind{ WirePacketKind::Data };
        WireChannel channel{ WireChannel::Unreliable };
        std::uint32_t sequence{};
        std::uint32_t ackSequence{};
        std::vector<ReplicationMessage> messages;
    };

    std::vector<std::uint8_t> SerializeWirePacket(const WirePacket& a_packet);
    WirePacket DeserializeWirePacket(const std::vector<std::uint8_t>& a_bytes);

    void RunWireProtocolSelfTest(const ReplicationFrame& a_frame);
}
