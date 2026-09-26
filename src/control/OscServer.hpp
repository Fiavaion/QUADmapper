#pragma once

#include "ControlEventQueue.hpp"
#include "core/Result.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace quadmapper::control {

// OSC 1.0 input over a Winsock UDP socket on its own thread (ADR-048 §1). Every
// datagram is parsed by parseOscPacket; its messages are pushed to the queue as
// ControlEvents, malformed datagrams are dropped and counted. Off until start().
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
};

}  // namespace quadmapper::control
