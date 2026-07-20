#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rudp {

constexpr std::uint32_t kMagic = 0x52554450U; // "RUDP"
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kMaxDatagramSize = 32768;

enum class PacketType : std::uint8_t {
    Start = 1,
    Data = 2,
    Fin = 3,
    Ack = 4,
    Error = 5,
};

struct Packet {
    PacketType type{PacketType::Data};
    std::uint16_t flags{0};
    std::uint32_t session_id{0};
    std::uint32_t sequence{0};
    std::uint32_t acknowledgement{0};
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_packet(const Packet& packet);
bool decode_packet(const std::uint8_t* data, std::size_t size, Packet& packet, std::string& error);
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);
const char* packet_type_name(PacketType type);

} // namespace rudp
