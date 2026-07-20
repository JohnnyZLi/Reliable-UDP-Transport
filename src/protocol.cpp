#include "protocol.hpp"

#include <array>
#include <stdexcept>

namespace rudp {
namespace {

void write_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out.at(offset) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    out.at(offset + 1) = static_cast<std::uint8_t>(value & 0xffU);
}

void write_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    out.at(offset) = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    out.at(offset + 1) = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    out.at(offset + 2) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    out.at(offset + 3) = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t read_u16(const std::uint8_t* data, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) |
                                     static_cast<std::uint16_t>(data[offset + 1]));
}

std::uint32_t read_u32(const std::uint8_t* data, std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> encode_packet(const Packet& packet) {
    if (packet.payload.size() > kMaxDatagramSize - kHeaderSize) {
        throw std::runtime_error("packet payload exceeds maximum datagram size");
    }

    std::vector<std::uint8_t> wire(kHeaderSize + packet.payload.size(), 0);
    write_u32(wire, 0, kMagic);
    wire[4] = kVersion;
    wire[5] = static_cast<std::uint8_t>(packet.type);
    write_u16(wire, 6, packet.flags);
    write_u32(wire, 8, packet.session_id);
    write_u32(wire, 12, packet.sequence);
    write_u32(wire, 16, packet.acknowledgement);
    write_u32(wire, 20, static_cast<std::uint32_t>(packet.payload.size()));
    write_u32(wire, 24, 0);
    write_u32(wire, 28, 0);
    for (std::size_t i = 0; i < packet.payload.size(); ++i) {
        wire[kHeaderSize + i] = packet.payload[i];
    }
    write_u32(wire, 24, crc32(wire.data(), wire.size()));
    return wire;
}

bool decode_packet(const std::uint8_t* data, std::size_t size, Packet& packet, std::string& error) {
    if (size < kHeaderSize) {
        error = "datagram shorter than protocol header";
        return false;
    }
    if (read_u32(data, 0) != kMagic) {
        error = "bad protocol magic";
        return false;
    }
    if (data[4] != kVersion) {
        error = "unsupported protocol version";
        return false;
    }
    const auto payload_size = static_cast<std::size_t>(read_u32(data, 20));
    if (payload_size != size - kHeaderSize) {
        error = "payload length does not match datagram size";
        return false;
    }

    std::vector<std::uint8_t> copy(data, data + size);
    const std::uint32_t expected = read_u32(copy.data(), 24);
    write_u32(copy, 24, 0);
    if (crc32(copy.data(), copy.size()) != expected) {
        error = "checksum mismatch";
        return false;
    }

    const auto type_value = data[5];
    if (type_value < static_cast<std::uint8_t>(PacketType::Start) ||
        type_value > static_cast<std::uint8_t>(PacketType::Error)) {
        error = "unknown packet type";
        return false;
    }

    packet.type = static_cast<PacketType>(type_value);
    packet.flags = read_u16(data, 6);
    packet.session_id = read_u32(data, 8);
    packet.sequence = read_u32(data, 12);
    packet.acknowledgement = read_u32(data, 16);
    packet.payload.assign(data + kHeaderSize, data + size);
    return true;
}

const char* packet_type_name(PacketType type) {
    switch (type) {
        case PacketType::Start: return "START";
        case PacketType::Data: return "DATA";
        case PacketType::Fin: return "FIN";
        case PacketType::Ack: return "ACK";
        case PacketType::Error: return "ERROR";
    }
    return "UNKNOWN";
}

} // namespace rudp
