#pragma once

#include "ControlEventQueue.hpp"
#include "core/Result.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace quadmapper::control {

// OSC 1.0 input over a Winsock UDP socket on its own thread (ADR-048 §1). Every
// datagram is parsed by parseOscPacket; its messages are pushed to the queue as
// ControlEvents, malformed datagrams are dropped and counted. Off until start().
// The same socket sends OSC feedback (sendTo), so a controller sees one peer.
//
// Shutdown never hangs: the receive loop waits in select() with a short timeout and
// checks the stop flag between waits, so stop() joins within ~kPollMs and only then
// closes the socket (no socket is closed under a thread still using it).
class OscServer {
public:
    static constexpr int kPollMs = 100;

    explicit OscServer(ControlEventQueue& queue);
    ~OscServer();

    OscServer(const OscServer&) = delete;
    OscServer& operator=(const OscServer&) = delete;

    // Bind `bindAddress` (dotted IPv4, "0.0.0.0" = every interface) and `port`
    // (0 = any free port, see boundPort()) and start receiving. A running server is
    // stopped first. The error says why it failed, e.g. the port is already in use.
    Result<void, Error> start(const std::string& bindAddress, int port);
    void stop();

    bool isRunning() const { return running_; }
    int boundPort() const { return boundPort_; }

    uint64_t packetsReceived() const { return packets_.load(std::memory_order_relaxed); }
    uint64_t malformedPackets() const { return malformed_.load(std::memory_order_relaxed); }
    uint64_t messagesReceived() const { return messages_.load(std::memory_order_relaxed); }

    // Where the last well-formed packet came from (OSC feedback replies there).
    struct Endpoint {
        uint32_t ip = 0;     // IPv4, host byte order
        uint16_t port = 0;
        bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
        bool operator!=(const Endpoint& o) const { return !(*this == o); }
    };
    std::optional<Endpoint> lastSender() const;

    // Send one datagram from the listening socket (OSC feedback, ADR-048 amendment).
    // Main thread only: start/stop run there too, so the socket cannot close under
    // it. False when the server is not running or sendto fails (counted).
    bool sendTo(const Endpoint& to, const std::vector<uint8_t>& datagram);
    uint64_t packetsSent() const { return sent_; }
    uint64_t sendErrors() const { return sendErrors_; }
    const std::string& lastSendError() const { return lastSendError_; }

    // "a.b.c.d:port" for the panel and logs.
    static std::string toString(const Endpoint& endpoint);
    // Parse a dotted IPv4 address (no host names: resolving one could block).
    static std::optional<uint32_t> parseIpv4(const std::string& text);

private:
    void receiveLoop();
    void closeSocket();

    ControlEventQueue& queue_;
    uintptr_t socket_;          // SOCKET; INVALID_SOCKET when closed
    bool winsockStarted_ = false;
    bool running_ = false;
    int boundPort_ = 0;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> malformed_{0};
    std::atomic<uint64_t> messages_{0};
    // (ip << 16 | port) | kHaveSender; 0 = nobody has sent yet.
    static constexpr uint64_t kHaveSender = 1ull << 48;
    std::atomic<uint64_t> lastSender_{0};
    uint64_t sent_ = 0;
    uint64_t sendErrors_ = 0;
    std::string lastSendError_;
};

}  // namespace quadmapper::control
