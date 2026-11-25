#include "control/OscServer.hpp"

#include "OscTestPackets.hpp"

#include <gtest/gtest.h>

#include "UdpTestSocket.hpp"

#include <chrono>
#include <thread>

using namespace quadmapper::control;
using quadmapper::control::test::OscPacket;
using quadmapper::control::test::oscMessage;

namespace {

// Sends UDP datagrams to 127.0.0.1 from the test process.
class UdpSender {
public:
    UdpSender() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    ~UdpSender() {
        closesocket(socket_);
        WSACleanup();
    }
    bool send(int port, const std::vector<uint8_t>& bytes) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        return sendto(socket_, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr*>(&to),
                      sizeof(to)) == static_cast<int>(bytes.size());
    }

private:
    SOCKET socket_;
};

// Wait until `count` events are queued, then drain them. Up to 10 s (ctest -j on a
// loaded machine); it returns as soon as they are in.
std::vector<ControlEvent> waitForEvents(ControlEventQueue& queue, size_t count) {
    std::vector<ControlEvent> out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (out.size() < count && std::chrono::steady_clock::now() < deadline) {
        queue.drain(out, 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out;
}

}  // namespace

TEST(OscServerTest, IsOffUntilStarted) {
    ControlEventQueue queue;
    OscServer server(queue);
    EXPECT_FALSE(server.isRunning());
    EXPECT_EQ(server.boundPort(), 0);
    server.stop();  // harmless when not running
}

TEST(OscServerTest, ReceivesMessagesAndBundlesFromLoopback) {
    ControlEventQueue queue;
    OscServer server(queue);
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    ASSERT_TRUE(server.isRunning());
    const int port = server.boundPort();
    ASSERT_GT(port, 0);

    UdpSender sender;
    auto blackout = oscMessage("/qm/blackout");
    blackout.string(",i").int32(1);
    ASSERT_TRUE(sender.send(port, blackout.bytes()));

    auto take = oscMessage("/qm/scene/take");
    take.string(",i").int32(2);
    auto fade = oscMessage("/qm/quad/Left/opacity");
    fade.string(",f").float32(0.5f);
    OscPacket bundle;
    bundle.bundleHeader().element(take).element(fade);
    ASSERT_TRUE(sender.send(port, bundle.bytes()));

    const auto events = waitForEvents(queue, 3);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].source, ControlEvent::Source::Osc);
    EXPECT_EQ(events[0].osc.address, "/qm/blackout");
    EXPECT_EQ(events[0].osc.args[0].i, 1);
    EXPECT_EQ(events[1].osc.address, "/qm/scene/take");
    EXPECT_EQ(events[1].osc.args[0].i, 2);
    EXPECT_EQ(events[2].osc.address, "/qm/quad/Left/opacity");
    EXPECT_FLOAT_EQ(events[2].osc.args[0].f, 0.5f);
    EXPECT_EQ(server.packetsReceived(), 2u);
    EXPECT_EQ(server.messagesReceived(), 3u);
    EXPECT_EQ(server.malformedPackets(), 0u);
}

TEST(OscServerTest, MalformedDatagramsAreCountedAndDropped) {
    ControlEventQueue queue;
    OscServer server(queue);
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    UdpSender sender;
    ASSERT_TRUE(sender.send(server.boundPort(), {'n', 'o', 'p', 'e'}));
    ASSERT_TRUE(sender.send(server.boundPort(), {'/', 'a'}));
    ASSERT_TRUE(sender.send(server.boundPort(), oscMessage("/ok").bytes()));
    const auto events = waitForEvents(queue, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].osc.address, "/ok");
    EXPECT_EQ(server.packetsReceived(), 3u);
    EXPECT_EQ(server.malformedPackets(), 2u);
}

TEST(OscServerTest, ReportsAPortAlreadyInUse) {
    ControlEventQueue queue;
    OscServer first(queue);
    ASSERT_TRUE(first.start("127.0.0.1", 0));
    OscServer second(queue);
    auto result = second.start("127.0.0.1", first.boundPort());
    ASSERT_FALSE(result);
    EXPECT_FALSE(second.isRunning());
    EXPECT_NE(result.error().message.find("in use"), std::string::npos) << result.error().message;
}

TEST(OscServerTest, RejectsInvalidBindAddressAndPort) {
    ControlEventQueue queue;
    OscServer server(queue);
    EXPECT_FALSE(server.start("not-an-ip", 9000));
    EXPECT_FALSE(server.start("127.0.0.1", 70000));
    EXPECT_FALSE(server.start("127.0.0.1", -1));
    EXPECT_FALSE(server.isRunning());
}

TEST(OscServerTest, StopIsPromptAndRestartWorks) {
    ControlEventQueue queue;
    OscServer server(queue);
    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(server.start("127.0.0.1", 0));
        const auto begin = std::chrono::steady_clock::now();
        server.stop();
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
        EXPECT_FALSE(server.isRunning());
    }
    // A restarted server receives again.
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    UdpSender sender;
    ASSERT_TRUE(sender.send(server.boundPort(), oscMessage("/again").bytes()));
    EXPECT_EQ(waitForEvents(queue, 1).size(), 1u);
}

TEST(OscServerTest, DestructorStopsARunningServer) {
    ControlEventQueue queue;
    int port = 0;
    {
        OscServer server(queue);
        ASSERT_TRUE(server.start("127.0.0.1", 0));
        port = server.boundPort();
    }
    // The port is free again once the server is gone.
    OscServer again(queue);
    EXPECT_TRUE(again.start("127.0.0.1", port));
}

TEST(OscServerTest, FloodIsBoundedByTheQueue) {
    ControlEventQueue queue(64);
    OscServer server(queue);
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    UdpSender sender;
    auto m = oscMessage("/flood");
    for (int n = 0; n < 500; ++n) sender.send(server.boundPort(), m.bytes());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.packetsReceived() < 500 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server.stop();  // no packet is half-processed while the counters are compared
    EXPECT_GT(server.messagesReceived(), 64u);
    EXPECT_LE(queue.size(), 64u);
    EXPECT_EQ(queue.size() + queue.droppedCount(), server.messagesReceived());
}
