#include "protocol.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Endpoint {
    sockaddr_storage address{};
    socklen_t length{0};
};

struct SessionKey {
    std::string endpoint;
    std::uint32_t session_id{0};

    bool operator==(const SessionKey& other) const {
        return endpoint == other.endpoint && session_id == other.session_id;
    }
};

struct SessionKeyHash {
    std::size_t operator()(const SessionKey& key) const {
        return std::hash<std::string>{}(key.endpoint) ^
               (static_cast<std::size_t>(key.session_id) << 1U);
    }
};

struct Session {
    std::filesystem::path final_path;
    std::filesystem::path temporary_path;
    std::ofstream output;
    std::uint32_t next_write_sequence{1};
    std::map<std::uint32_t, std::vector<std::uint8_t>> pending;
    std::optional<std::uint32_t> fin_sequence;
    Clock::time_point last_seen{Clock::now()};
    bool completed{false};
};

void usage() {
    std::cerr << "Usage: myserver port_number droppc\n";
}

bool parse_long(const char* text, long minimum, long maximum, long& value) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

int bind_udp(const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(nullptr, port.c_str(), &hints, &results);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));
    }

    int fd = -1;
    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        throw std::runtime_error(rudp::errno_message("unable to bind UDP socket"));
    }
    return fd;
}

std::string endpoint_string(const sockaddr_storage& address, socklen_t length) {
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    const int rc = getnameinfo(reinterpret_cast<const sockaddr*>(&address), length,
                               host, sizeof(host), service, sizeof(service),
                               NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        return "unknown";
    }
    return std::string(host) + ":" + service;
}

bool safe_relative_path(const std::string& text, std::filesystem::path& result) {
    if (text.empty()) {
        return false;
    }
    std::filesystem::path path(text);
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }
    result = path.lexically_normal();
    return !result.empty() && result != ".";
}

void log_server(const char* kind, std::uint32_t sequence) {
    std::cout << rudp::rfc3339_now() << ", " << kind << ", " << sequence << '\n';
}

bool finish_if_ready(Session& session) {
    if (!session.fin_sequence.has_value() ||
        session.next_write_sequence != session.fin_sequence.value() ||
        session.completed) {
        return false;
    }
    session.output.flush();
    session.output.close();
    std::error_code error;
    std::filesystem::rename(session.temporary_path, session.final_path, error);
    if (error) {
        std::filesystem::remove(session.final_path, error);
        error.clear();
        std::filesystem::rename(session.temporary_path, session.final_path, error);
    }
    if (error) {
        return false;
    }
    session.completed = true;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        usage();
        return 2;
    }
    long port = 0;
    long drop_percent = 0;
    if (!parse_long(argv[1], 1, 65535, port) || !parse_long(argv[2], 0, 100, drop_percent)) {
        usage();
        return 2;
    }

    int socket_fd = -1;
    try {
        socket_fd = bind_udp(std::to_string(port));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }

    std::uint32_t seed = rudp::random_u32();
    if (const char* seed_text = std::getenv("RUDP_DROP_SEED")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(seed_text, &end, 10);
        if (end != seed_text && *end == '\0') {
            seed = static_cast<std::uint32_t>(parsed);
        }
    }
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> percentage(1, 100);
    auto should_drop = [&]() {
        return drop_percent > 0 && percentage(random) <= drop_percent;
    };

    std::unordered_map<SessionKey, Session, SessionKeyHash> sessions;
    std::vector<std::uint8_t> buffer(rudp::kMaxDatagramSize);

    auto send_packet = [&](const Endpoint& endpoint, const rudp::Packet& packet) {
        if (should_drop()) {
            log_server("DROP ACK", packet.acknowledgement);
            return;
        }
        const auto wire = rudp::encode_packet(packet);
        const ssize_t count = sendto(socket_fd, wire.data(), wire.size(), 0,
                                     reinterpret_cast<const sockaddr*>(&endpoint.address),
                                     endpoint.length);
        if (count < 0) {
            std::cerr << rudp::errno_message("sendto") << '\n';
            return;
        }
        log_server("ACK", packet.acknowledgement);
    };

    while (true) {
        pollfd descriptor{};
        descriptor.fd = socket_fd;
        descriptor.events = POLLIN;
        const int rc = poll(&descriptor, 1, 1000);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << rudp::errno_message("poll") << '\n';
            break;
        }

        const auto now = Clock::now();
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (now - it->second.last_seen > std::chrono::minutes(2)) {
                if (it->second.output.is_open()) {
                    it->second.output.close();
                }
                if (!it->second.completed) {
                    std::error_code ignored;
                    std::filesystem::remove(it->second.temporary_path, ignored);
                }
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }

        if (rc == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }

        Endpoint endpoint;
        endpoint.length = sizeof(endpoint.address);
        const ssize_t count = recvfrom(socket_fd, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr*>(&endpoint.address),
                                       &endpoint.length);
        if (count < 0) {
            std::cerr << rudp::errno_message("recvfrom") << '\n';
            continue;
        }

        rudp::Packet packet;
        std::string decode_error;
        if (!rudp::decode_packet(buffer.data(), static_cast<std::size_t>(count), packet, decode_error)) {
            std::cerr << "Ignoring invalid datagram: " << decode_error << '\n';
            continue;
        }

        if (should_drop()) {
            const char* label = packet.type == rudp::PacketType::Ack ? "DROP ACK" : "DROP DATA";
            log_server(label, packet.sequence);
            continue;
        }
        if (packet.type == rudp::PacketType::Data) {
            log_server("DATA", packet.sequence);
        }

        const SessionKey key{endpoint_string(endpoint.address, endpoint.length), packet.session_id};
        auto session_it = sessions.find(key);

        if (packet.type == rudp::PacketType::Start) {
            std::filesystem::path path;
            const std::string path_text(packet.payload.begin(), packet.payload.end());
            if (!safe_relative_path(path_text, path)) {
                rudp::Packet error;
                error.type = rudp::PacketType::Error;
                error.session_id = packet.session_id;
                const std::string message = "unsafe or invalid output path";
                error.payload.assign(message.begin(), message.end());
                const auto wire = rudp::encode_packet(error);
                sendto(socket_fd, wire.data(), wire.size(), 0,
                       reinterpret_cast<const sockaddr*>(&endpoint.address), endpoint.length);
                continue;
            }

            if (session_it == sessions.end()) {
                std::error_code directory_error;
                const auto parent = path.parent_path();
                if (!parent.empty()) {
                    std::filesystem::create_directories(parent, directory_error);
                }
                Session session;
                session.final_path = path;
                session.temporary_path = path;
                session.temporary_path += ".rudp." + std::to_string(packet.session_id) + ".part";
                session.output.open(session.temporary_path, std::ios::binary | std::ios::trunc);
                if (!session.output) {
                    rudp::Packet error;
                    error.type = rudp::PacketType::Error;
                    error.session_id = packet.session_id;
                    const std::string message = "cannot create output file";
                    error.payload.assign(message.begin(), message.end());
                    const auto wire = rudp::encode_packet(error);
                    sendto(socket_fd, wire.data(), wire.size(), 0,
                           reinterpret_cast<const sockaddr*>(&endpoint.address), endpoint.length);
                    continue;
                }
                auto inserted = sessions.emplace(key, std::move(session));
                session_it = inserted.first;
            }
        }

        if (session_it == sessions.end()) {
            continue;
        }
        Session& session = session_it->second;
        session.last_seen = now;

        if (packet.type == rudp::PacketType::Data && !session.completed) {
            if (packet.sequence >= session.next_write_sequence &&
                session.pending.find(packet.sequence) == session.pending.end()) {
                session.pending.emplace(packet.sequence, packet.payload);
            }
            while (true) {
                auto pending = session.pending.find(session.next_write_sequence);
                if (pending == session.pending.end()) {
                    break;
                }
                session.output.write(reinterpret_cast<const char*>(pending->second.data()),
                                     static_cast<std::streamsize>(pending->second.size()));
                if (!session.output) {
                    std::cerr << "write failed for " << session.temporary_path << '\n';
                    break;
                }
                session.pending.erase(pending);
                ++session.next_write_sequence;
            }
            finish_if_ready(session);
        } else if (packet.type == rudp::PacketType::Fin) {
            session.fin_sequence = packet.sequence;
            finish_if_ready(session);
        }

        if (packet.type == rudp::PacketType::Start ||
            packet.type == rudp::PacketType::Data ||
            packet.type == rudp::PacketType::Fin) {
            rudp::Packet ack;
            ack.type = rudp::PacketType::Ack;
            ack.session_id = packet.session_id;
            ack.acknowledgement = packet.sequence;
            send_packet(endpoint, ack);
        }
    }

    close(socket_fd);
    return 2;
}
