#include "OscServer.hpp"

#include "OscParser.hpp"
#include "core/Logger.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET  // mstcpip.h defines it only for some _WIN32_WINNT targets
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <chrono>
#include <vector>

namespace quadmapper::control {

namespace {

std::string winsockErrorText(int code) {
    switch (code) {
        case WSAEADDRINUSE:     return "the port is already in use by another program";
        case WSAEACCES:         return "access to the port was denied (another program may hold it exclusively)";
        case WSAEADDRNOTAVAIL:  return "the bind address is not an address of this computer";
        case WSANOTINITIALISED: return "networking is not initialised";
        default:                return "Winsock error " + std::to_string(code);
    }
}

}  // namespace

OscServer::OscServer(ControlEventQueue& queue)
    : queue_(queue), socket_(static_cast<uintptr_t>(INVALID_SOCKET)) {}

OscServer::~OscServer() {
    stop();
}

Result<void, Error> OscServer::start(const std::string& bindAddress, int port) {
    stop();
    auto fail = [this](const std::string& message) {
        closeSocket();
        Logger::warning("OSC: {}", message);
        return Result<void, Error>::error(Error(message));
    };

    if (port < 0 || port > 65535) return fail("port " + std::to_string(port) + " is out of range (0-65535)");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    const std::string host = bindAddress.empty() ? std::string("0.0.0.0") : bindAddress;
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        return fail("'" + host + "' is not a valid IPv4 address");
    }

    WSADATA wsaData;
    const int startup = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startup != 0) return fail("could not start networking (" + winsockErrorText(startup) + ")");
    winsockStarted_ = true;

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return fail("could not create a UDP socket (" + winsockErrorText(WSAGetLastError()) + ")");
    socket_ = static_cast<uintptr_t>(s);

    // Exclusive: another program cannot bind the same port behind our back, and a
    // port someone else holds is reported instead of silently shared.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    int receiveBuffer = 1 << 20;  // absorb fader bursts while the main thread renders
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer),
               sizeof(receiveBuffer));
    // Feedback sent to a port nobody listens on comes back as ICMP "port unreachable",
    // which Windows reports on the next recvfrom as WSAECONNRESET; switch that off.
    BOOL reportReset = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned,
             nullptr, nullptr);

    if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        return fail("cannot listen on UDP " + host + ":" + std::to_string(port) + ": " +
                    winsockErrorText(WSAGetLastError()));
    }

    sockaddr_in bound{};
    int boundLength = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
        boundPort_ = ntohs(bound.sin_port);
    } else {
        boundPort_ = port;
    }

    stopRequested_ = false;
    lastSender_ = 0;
    try {
        thread_ = std::thread(&OscServer::receiveLoop, this);
    } catch (const std::exception& e) {
        return fail(std::string("could not start the receive thread: ") + e.what());
    }
    running_ = true;
    Logger::info("OSC: listening on UDP {}:{}", host, boundPort_);
    return Result<void, Error>::success();
}

void OscServer::stop() {
    if (thread_.joinable()) {
        stopRequested_ = true;
        thread_.join();
    }
    const bool wasRunning = running_;
    running_ = false;
    closeSocket();
    if (wasRunning) Logger::info("OSC: stopped");
}

void OscServer::closeSocket() {
    if (socket_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
    if (winsockStarted_) {
        WSACleanup();
        winsockStarted_ = false;
    }
    boundPort_ = 0;
}

std::optional<OscServer::Endpoint> OscServer::lastSender() const {
    const uint64_t packed = lastSender_.load(std::memory_order_relaxed);
    if ((packed & kHaveSender) == 0) return std::nullopt;
    return Endpoint{static_cast<uint32_t>((packed >> 16) & 0xFFFFFFFFu),
                    static_cast<uint16_t>(packed & 0xFFFFu)};
}

bool OscServer::sendTo(const Endpoint& to, const std::vector<uint8_t>& datagram) {
    if (!running_ || socket_ == static_cast<uintptr_t>(INVALID_SOCKET)) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(to.port);
    address.sin_addr.s_addr = htonl(to.ip);
    const int sent = sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(datagram.data()),
                            static_cast<int>(datagram.size()), 0,
                            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent != static_cast<int>(datagram.size())) {
        ++sendErrors_;
        const std::string reason = "cannot send to " + toString(to) + ": " +
                                   winsockErrorText(sent == SOCKET_ERROR ? WSAGetLastError() : 0);
        if (reason != lastSendError_) Logger::warning("OSC: {}", reason);  // once per distinct error
        lastSendError_ = reason;
        return false;
    }
    ++sent_;
    return true;
}

std::string OscServer::toString(const Endpoint& endpoint) {
    return std::to_string((endpoint.ip >> 24) & 0xFF) + "." + std::to_string((endpoint.ip >> 16) & 0xFF) +
           "." + std::to_string((endpoint.ip >> 8) & 0xFF) + "." + std::to_string(endpoint.ip & 0xFF) +
           ":" + std::to_string(endpoint.port);
}

std::optional<uint32_t> OscServer::parseIpv4(const std::string& text) {
    in_addr address{};
    if (text.empty() || inet_pton(AF_INET, text.c_str(), &address) != 1) return std::nullopt;
    return ntohl(address.s_addr);
}

void OscServer::receiveLoop() {
    const SOCKET s = static_cast<SOCKET>(socket_);
    std::vector<uint8_t> buffer(65536);  // the largest UDP payload
    std::vector<OscMessage> messages;

    while (!stopRequested_) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval timeout{0, kPollMs * 1000};
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == 0) continue;
        if (ready == SOCKET_ERROR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }

        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(s, reinterpret_cast<char*>(buffer.data()),
                                      static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEMSGSIZE) {  // truncated oversize datagram
                packets_.fetch_add(1, std::memory_order_relaxed);
                malformed_.fetch_add(1, std::memory_order_relaxed);
            } else if (error != WSAECONNRESET) {
                // Unexpected: back off rather than spin on a failing socket.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }

        packets_.fetch_add(1, std::memory_order_relaxed);
        messages.clear();
        if (!parseOscPacket(buffer.data(), static_cast<size_t>(received), messages)) {
            malformed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint32_t senderIp = ntohl(from.sin_addr.s_addr);
        const uint16_t senderPort = ntohs(from.sin_port);
        if (fromLength >= static_cast<int>(sizeof(from)) && from.sin_family == AF_INET) {
            lastSender_.store((static_cast<uint64_t>(senderIp) << 16) | senderPort | kHaveSender,
                              std::memory_order_relaxed);
        }
        for (auto& message : messages) {
            ControlEvent event;
            event.source = ControlEvent::Source::Osc;
            event.osc = std::move(message);
            event.oscSenderIp = senderIp;
            event.oscSenderPort = senderPort;
            queue_.push(std::move(event));
            messages_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace quadmapper::control
