#include "protocol.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Slot {
    rudp::Packet packet;
    std::vector<std::uint8_t> wire;
    Clock::time_point last_sent{};
    int retransmissions{0};
    bool acknowledged{false};
};

void usage() {
    std::cerr << "Usage: myclient server_ip server_port mss winsz in_file_path out_file_path\n";
}

bool parse_positive(const char* text, long minimum, long maximum, long& value) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

int connect_udp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));
    }

    int fd = -1;
    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        throw std::runtime_error(rudp::errno_message("unable to connect UDP socket"));
    }
    return fd;
}

void log_client(const char* kind, std::uint32_t packet_number,
                std::uint32_t base, std::uint32_t next, std::uint32_t window_size) {
    std::cout << rudp::rfc3339_now() << ", " << kind << ", " << packet_number << ", "
              << base << ", " << next << ", " << (base + window_size) << '\n';
}

rudp::Packet make_packet(std::uint32_t session_id, std::uint32_t seq,
                         std::uint32_t data_packets, std::size_t chunk_size,
                         const std::string& output_path, std::ifstream& input,
                         std::uint64_t file_size) {
    rudp::Packet packet;
    packet.session_id = session_id;
    packet.sequence = seq;

    if (seq == 0) {
        packet.type = rudp::PacketType::Start;
        packet.payload.assign(output_path.begin(), output_path.end());
        return packet;
    }
    if (seq == data_packets + 1U) {
        packet.type = rudp::PacketType::Fin;
        return packet;
    }

    packet.type = rudp::PacketType::Data;
    const std::uint64_t offset = static_cast<std::uint64_t>(seq - 1U) * chunk_size;
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(chunk_size, file_size - offset));
    packet.payload.resize(wanted);
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input.good()) {
        throw std::runtime_error("failed to seek input file");
    }
    input.read(reinterpret_cast<char*>(packet.payload.data()), static_cast<std::streamsize>(wanted));
    if (static_cast<std::size_t>(input.gcount()) != wanted) {
        throw std::runtime_error("failed to read input file");
    }
    return packet;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        usage();
        return 2;
    }

    long mss_value = 0;
    long window_value = 0;
    if (!parse_positive(argv[3], 1, static_cast<long>(rudp::kMaxDatagramSize), mss_value) ||
        !parse_positive(argv[4], 1, 65535, window_value)) {
        usage();
        return 2;
    }
    if (mss_value <= static_cast<long>(rudp::kHeaderSize)) {
        std::cerr << "Required minimum MSS is " << (rudp::kHeaderSize + 1U) << '\n';
        return 1;
    }

    const std::string input_path = argv[5];
    const std::string output_path = argv[6];
    if (output_path.empty() || output_path.size() > mss_value - rudp::kHeaderSize) {
        std::cerr << "Output path is empty or too long for the selected MSS\n";
        return 2;
    }

    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(input_path, file_error);
    if (file_error) {
        std::cerr << "Cannot read input file: " << file_error.message() << '\n';
        return 2;
    }
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open input file\n";
        return 2;
    }

    const std::size_t chunk_size = static_cast<std::size_t>(mss_value) - rudp::kHeaderSize;
    const std::uint64_t data_packet_count64 =
        (file_size + chunk_size - 1U) / chunk_size;
    if (data_packet_count64 > std::numeric_limits<std::uint32_t>::max() - 2U) {
        std::cerr << "Input file is too large for the 32-bit sequence space\n";
        return 2;
    }
    const auto data_packet_count = static_cast<std::uint32_t>(data_packet_count64);
    const std::uint32_t final_sequence = data_packet_count + 1U;
    const std::uint32_t window_size = static_cast<std::uint32_t>(window_value);
    const std::uint32_t session_id = rudp::random_u32();

    int socket_fd = -1;
    try {
        socket_fd = connect_udp(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }

    const long retransmit_ms = rudp::env_milliseconds("RUDP_RETRANSMIT_TIMEOUT_MS", 750);
    const long server_timeout_ms = rudp::env_milliseconds("RUDP_SERVER_TIMEOUT_MS", 30000);
    const auto retransmit_timeout = std::chrono::milliseconds(retransmit_ms);
    const auto server_timeout = std::chrono::milliseconds(server_timeout_ms);

    std::map<std::uint32_t, Slot> window;
    std::uint32_t base = 0;
    std::uint32_t next = 0;
    bool received_any = false;
    auto last_receive = Clock::now();
    const auto started = Clock::now();
    std::vector<std::uint8_t> receive_buffer(rudp::kMaxDatagramSize);

    auto send_slot = [&](Slot& slot, bool retransmission) -> bool {
        const ssize_t sent = send(socket_fd, slot.wire.data(), slot.wire.size(), 0);
        if (sent < 0) {
            if (errno == ECONNREFUSED || errno == EHOSTUNREACH || errno == ENETUNREACH) {
                return false;
            }
            std::cerr << rudp::errno_message("send") << '\n';
            return false;
        }
        if (static_cast<std::size_t>(sent) != slot.wire.size()) {
            std::cerr << "short UDP send\n";
            return false;
        }
        slot.last_sent = Clock::now();
        if (slot.packet.type == rudp::PacketType::Data) {
            log_client("DATA", slot.packet.sequence, base, next, window_size);
        }
        if (retransmission) {
            std::cerr << "Packet loss detected\n";
        }
        return true;
    };

    int exit_code = 0;
    while (base <= final_sequence) {
        while (next <= final_sequence && next < base + window_size) {
            Slot slot;
            try {
                slot.packet = make_packet(session_id, next, data_packet_count, chunk_size,
                                          output_path, input, file_size);
                slot.wire = rudp::encode_packet(slot.packet);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                exit_code = 2;
                goto done;
            }
            auto [it, inserted] = window.emplace(next, std::move(slot));
            (void) inserted;
            ++next;
            if (!send_slot(it->second, false)) {
                exit_code = received_any ? 5 : 3;
                std::cerr << (received_any ? "Server is down" : "Cannot detect server") << '\n';
                goto done;
            }
        }

        pollfd descriptor{};
        descriptor.fd = socket_fd;
        descriptor.events = POLLIN;
        const int poll_result = poll(&descriptor, 1, 50);
        const auto now = Clock::now();
        if (poll_result < 0 && errno != EINTR) {
            std::cerr << rudp::errno_message("poll") << '\n';
            exit_code = 2;
            goto done;
        }
        if (poll_result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::cerr << (received_any ? "Server is down" : "Cannot detect server") << '\n';
            exit_code = received_any ? 5 : 3;
            goto done;
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t count = recv(socket_fd, receive_buffer.data(), receive_buffer.size(), 0);
            if (count < 0) {
                if (errno == ECONNREFUSED || errno == EHOSTUNREACH || errno == ENETUNREACH) {
                    std::cerr << (received_any ? "Server is down" : "Cannot detect server") << '\n';
                    exit_code = received_any ? 5 : 3;
                    goto done;
                }
            } else {
                rudp::Packet packet;
                std::string decode_error;
                if (rudp::decode_packet(receive_buffer.data(), static_cast<std::size_t>(count),
                                        packet, decode_error) &&
                    packet.session_id == session_id) {
                    received_any = true;
                    last_receive = now;
                    if (packet.type == rudp::PacketType::Error) {
                        std::cerr << "Server error: "
                                  << std::string(packet.payload.begin(), packet.payload.end()) << '\n';
                        exit_code = 2;
                        goto done;
                    }
                    if (packet.type == rudp::PacketType::Ack) {
                        const auto ack = packet.acknowledgement;
                        log_client("ACK", ack, base, next, window_size);
                        auto it = window.find(ack);
                        if (it != window.end()) {
                            it->second.acknowledged = true;
                        }
                        while (true) {
                            auto first = window.find(base);
                            if (first == window.end() || !first->second.acknowledged) {
                                break;
                            }
                            window.erase(first);
                            ++base;
                        }
                    }
                }
            }
        }

        for (auto& [sequence, slot] : window) {
            (void) sequence;
            if (slot.acknowledged || now - slot.last_sent < retransmit_timeout) {
                continue;
            }
            if (slot.retransmissions >= 5) {
                std::cerr << "Reached max re-transmission limit\n";
                exit_code = 4;
                goto done;
            }
            ++slot.retransmissions;
            if (!send_slot(slot, true)) {
                std::cerr << (received_any ? "Server is down" : "Cannot detect server") << '\n';
                exit_code = received_any ? 5 : 3;
                goto done;
            }
        }

        if (!received_any && now - started >= server_timeout) {
            std::cerr << "Cannot detect server\n";
            exit_code = 3;
            goto done;
        }
        if (received_any && now - last_receive >= server_timeout) {
            std::cerr << "Server is down\n";
            exit_code = 5;
            goto done;
        }
    }

done:
    close(socket_fd);
    return exit_code;
}
