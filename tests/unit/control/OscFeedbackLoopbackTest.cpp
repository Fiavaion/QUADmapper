// OSC feedback end to end over UDP loopback: ExternalControl samples real targets,
// sends from its OSC socket, and a test socket (standing in for TouchOSC) receives
// and parses the packets with the same parser the app uses for input.

#include "control/ExternalControl.hpp"
#include "control/OscParser.hpp"
#include "control/OscWriter.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <map>
#include <thread>

using namespace quadmapper::control;

namespace {

constexpr const char* kLeft1 = "11111111-aaaa-bbbb-cccc-000000000001";
constexpr const char* kLeft2 = "11111111-aaaa-bbbb-cccc-000000000002";
constexpr const char* kWall = "11111111-aaaa-bbbb-cccc-000000000003";

// A controller on 127.0.0.1: one socket that both sends to the app and receives its
// feedback (as TouchOSC does).
class Peer {
public:
    Peer() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        int length = sizeof(address);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
    }
    ~Peer() {
        closesocket(socket_);
        WSACleanup();
    }
    int port() const { return port_; }

    void send(int port, const OscMessage& message) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(writeOscMessage(message, bytes));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        ASSERT_EQ(sendto(socket_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                         0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)),
                  static_cast<int>(bytes.size()));
    }

    // Every message received until `quietMs` pass with nothing new (or 3 s overall).
    std::vector<OscMessage> receive(int quietMs = 150) {
        std::vector<OscMessage> messages;
        std::vector<uint8_t> buffer(65536);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket_, &readable);
            timeval timeout{0, quietMs * 1000};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) break;
            const int n = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()), 0, nullptr, nullptr);
            if (n <= 0) break;
            EXPECT_TRUE(parseOscPacket(buffer.data(), static_cast<size_t>(n), messages))
                << "feedback packet did not parse";
        }
        return messages;
    }

private:
    SOCKET socket_;
    int port_ = 0;
};

OscArg f(float v) { OscArg a; a.type = OscArg::Type::Float; a.tag = 'f'; a.f = v; return a; }

OscMessage msg(const std::string& address, std::vector<OscArg> args) {
    OscMessage m;
    m.address = address;
    m.args = std::move(args);
    return m;
}

std::map<std::string, OscArg> byAddress(const std::vector<OscMessage>& messages) {
    std::map<std::string, OscArg> out;
    for (const auto& m : messages) {
        EXPECT_EQ(m.args.size(), 1u) << m.address;
        EXPECT_EQ(out.count(m.address), 0u) << m.address << " sent twice";
        if (!m.args.empty()) out[m.address] = m.args[0];
    }
    return out;
}

// A show with a few targets on real state.
struct Show {
    ExternalControl control;
    std::vector<MidiMapping> mappings;
    bool blackout = false;
    int scene = 1;
    std::map<std::string, float> opacity{{kLeft1, 0.25f}, {kLeft2, 0.5f}, {kWall, 1.0f}};

    Show() {
        ControlRegistry& r = control.registry();
        r.addGlobal("/qm/blackout", {"", TargetKind::Bool, 0, 1, true,
                                     [this](const std::string&, float v) { blackout = v >= 0.5f; },
                                     [this](const std::string&) { return std::optional<float>(blackout ? 1.f : 0.f); }});
        r.addGlobal("/qm/scene/active", {"", TargetKind::Index, 1, 1e6f, false,
                                         [this](const std::string&, float v) { scene = static_cast<int>(v); },
                                         [this](const std::string&) { return std::optional<float>(float(scene)); }});
        r.addGlobal("/qm/timeline/play", {"", TargetKind::Trigger, 0, 1, true,
                                          [](const std::string&, float) {}, nullptr});  // no feedback
        r.addEntityTarget("quad", {"opacity", TargetKind::Float, 0, 1, true,
                                   [this](const std::string& id, float v) { opacity[id] = v; },
                                   [this](const std::string& id) { return std::optional<float>(opacity[id]); }});
        // Two quads share a name (answered by id), one name has a space (by id), and
        // one is unique and valid (by name).
        r.setEntities("quad", {{kLeft1, "Left"}, {kLeft2, "left"}, {kWall, "My Wall"}});
        EXPECT_TRUE(control.startOsc("127.0.0.1", 0));
    }

    int oscPort() { return control.osc().boundPort(); }

    // Wait until the OSC thread queued `count` messages, then apply them.
    void process(size_t count, double now) {
        size_t applied = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (applied < count && std::chrono::steady_clock::now() < deadline) {
            applied += control.processEvents(mappings, now);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_EQ(applied, count);
    }
};

std::string quad(const char* id) { return std::string("/qm/quad/") + id + "/opacity"; }

}  // namespace

TEST(OscFeedbackIntegrationTest, SendsEveryValueToAFixedHostThenOnlyChanges) {
    Show show;
    Peer peer;
    show.control.setFeedback({true, "127.0.0.1", peer.port(), false});
    EXPECT_EQ(show.control.feedbackStatus(), ExternalControl::FeedbackStatus::Sending);

    show.control.updateFeedback(0.0);
    const auto first = byAddress(peer.receive());
    ASSERT_EQ(first.size(), 5u);
    EXPECT_EQ(first.at("/qm/blackout").type, OscArg::Type::Float);
    EXPECT_EQ(first.at("/qm/blackout").f, 0.0f);
    EXPECT_EQ(first.at("/qm/scene/active").type, OscArg::Type::Int);
    EXPECT_EQ(first.at("/qm/scene/active").i, 1);
    EXPECT_EQ(first.at(quad(kLeft1)).f, 0.25f);  // shared name: by id
    EXPECT_EQ(first.at(quad(kLeft2)).f, 0.5f);
    EXPECT_EQ(first.at(quad(kWall)).f, 1.0f);    // name with a space: by id
    EXPECT_EQ(show.control.osc().packetsSent(), 5u);
    EXPECT_EQ(show.control.feedbackAddressCount(), 5u);

    // A change goes out alone, and not before the rate limit allows.
    show.opacity[kLeft2] = 0.75f;
    show.blackout = true;
    show.control.updateFeedback(0.01);  // not due yet
    EXPECT_TRUE(peer.receive(60).empty());
    show.control.updateFeedback(0.05);
    const auto second = byAddress(peer.receive());
    ASSERT_EQ(second.size(), 2u);
    EXPECT_EQ(second.at(quad(kLeft2)).f, 0.75f);
    EXPECT_EQ(second.at("/qm/blackout").f, 1.0f);

    // Nothing changed: nothing sent.
    show.control.updateFeedback(1.0);
    EXPECT_TRUE(peer.receive(60).empty());

    // Unique valid names are used once the clash is gone.
    show.control.registry().setEntities("quad", {{kLeft1, "Left"}, {kLeft2, "Right"}, {kWall, "My Wall"}});
    show.control.updateFeedback(2.0);
    const auto renamed = byAddress(peer.receive());
    EXPECT_EQ(renamed.count("/qm/quad/Left/opacity"), 1u);
    EXPECT_EQ(renamed.count("/qm/quad/Right/opacity"), 1u);
    EXPECT_EQ(renamed.size(), 2u);

    // Send all.
    show.control.sendAllFeedback();
    show.control.updateFeedback(3.0);
    EXPECT_EQ(byAddress(peer.receive()).size(), 5u);
}

TEST(OscFeedbackIntegrationTest, RepliesToTheSenderAndDoesNotEchoItsOwnValues) {
    Show show;
    Peer peer;
    show.control.setFeedback({true, "", 0, false});  // reply to the sender's own port
    EXPECT_EQ(show.control.feedbackStatus(), ExternalControl::FeedbackStatus::WaitingForController);
    show.control.updateFeedback(0.0);
    EXPECT_TRUE(peer.receive(60).empty());

    // The controller's first message is its "connect": it gets every value.
    peer.send(show.oscPort(), msg("/qm/quad/" + std::string(kWall) + "/opacity", {f(0.4f)}));
    show.process(1, 1.0);
    EXPECT_EQ(show.opacity[kWall], 0.4f);
    EXPECT_EQ(show.control.feedbackStatus(), ExternalControl::FeedbackStatus::Sending);
    ASSERT_TRUE(show.control.feedbackDestination().has_value());
    EXPECT_EQ(show.control.feedbackDestination()->port, peer.port());
    show.control.updateFeedback(1.0);
    const auto connect = byAddress(peer.receive());
    ASSERT_EQ(connect.size(), 5u);
    EXPECT_EQ(connect.at(quad(kWall)).f, 0.4f);

    // A value the controller sets is not echoed back to it.
    peer.send(show.oscPort(), msg("/qm/quad/" + std::string(kWall) + "/opacity", {f(0.6f)}));
    show.process(1, 2.0);
    show.control.updateFeedback(2.0);
    EXPECT_TRUE(peer.receive(80).empty());

    // A value the target corrects (clamped to 1) is sent back.
    peer.send(show.oscPort(), msg("/qm/quad/" + std::string(kWall) + "/opacity", {f(3.0f)}));
    show.process(1, 3.0);
    show.control.updateFeedback(3.0);
    const auto corrected = byAddress(peer.receive());
    ASSERT_EQ(corrected.size(), 1u);
    EXPECT_EQ(corrected.at(quad(kWall)).f, 1.0f);

    // A pattern sets every quad; none of them is echoed.
    peer.send(show.oscPort(), msg("/qm/quad/*/opacity", {f(0.0f)}));
    show.process(1, 4.0);
    EXPECT_EQ(show.opacity[kLeft1], 0.0f);
    show.control.updateFeedback(4.0);
    EXPECT_TRUE(peer.receive(80).empty());

    // A change made elsewhere (the UI, MIDI) does reach the controller.
    show.scene = 3;
    show.control.updateFeedback(5.0);
    const auto scene = byAddress(peer.receive());
    ASSERT_EQ(scene.size(), 1u);
    EXPECT_EQ(scene.at("/qm/scene/active").i, 3);
}

TEST(OscFeedbackIntegrationTest, AFixedFeedbackPortOverridesTheSendersPort) {
    Show show;
    Peer controller;  // sends
    Peer listener;    // the controller's receive port
    show.control.setFeedback({true, "", listener.port(), true});
    controller.send(show.oscPort(), msg("/qm/blackout", {f(1.0f)}));
    show.process(1, 1.0);
    show.control.updateFeedback(1.0);
    const auto received = byAddress(listener.receive());
    EXPECT_EQ(received.size(), 5u);
    EXPECT_TRUE(controller.receive(60).empty());
    // useIds: every quad by id even when its name would do.
    show.control.registry().setEntities("quad", {{kLeft1, "A"}, {kLeft2, "B"}, {kWall, "C"}});
    show.control.updateFeedback(2.0);
    EXPECT_TRUE(listener.receive(60).empty()) << "same keys and addresses: nothing new";
}

TEST(OscFeedbackIntegrationTest, RefreshResendsEverythingToTheDestination) {
    Show show;
    Peer peer;
    show.control.setFeedback({true, "127.0.0.1", peer.port(), false});
    show.control.updateFeedback(0.0);
    ASSERT_EQ(peer.receive().size(), 5u);
    show.control.updateFeedback(1.0);
    EXPECT_TRUE(peer.receive(60).empty());

    // The controller restarted and asks for the state (no argument, or a press).
    OscMessage refresh;
    refresh.address = ExternalControl::kFeedbackRefreshPath;
    peer.send(show.oscPort(), refresh);
    show.process(1, 2.0);
    show.control.updateFeedback(2.0);
    EXPECT_EQ(byAddress(peer.receive()).size(), 5u);
    peer.send(show.oscPort(), msg(ExternalControl::kFeedbackRefreshPath, {f(1.0f)}));
    show.process(1, 3.0);
    show.control.updateFeedback(3.0);
    EXPECT_EQ(byAddress(peer.receive()).size(), 5u);
    EXPECT_EQ(show.control.dispatcher().unknownAddresses(), 0u);
    // The refresh itself is not a feedback address.
    EXPECT_EQ(show.control.feedbackAddressCount(), 5u);
}

TEST(OscFeedbackIntegrationTest, RefreshInReplyModeAnswersTheSender) {
    Show show;
    Peer first;
    Peer second;
    show.control.setFeedback({true, "", 0, false});
    first.send(show.oscPort(), msg("/qm/blackout", {f(0.0f)}));
    show.process(1, 1.0);
    show.control.updateFeedback(1.0);
    ASSERT_EQ(first.receive().size(), 5u);

    // The same controller asks again: it gets everything again.
    OscMessage refresh;
    refresh.address = ExternalControl::kFeedbackRefreshPath;
    first.send(show.oscPort(), refresh);
    show.process(1, 2.0);
    show.control.updateFeedback(2.0);
    EXPECT_EQ(byAddress(first.receive()).size(), 5u);

    // Another controller's refresh makes it the destination and it gets everything.
    second.send(show.oscPort(), refresh);
    show.process(1, 3.0);
    show.control.updateFeedback(3.0);
    EXPECT_EQ(byAddress(second.receive()).size(), 5u);
    EXPECT_TRUE(first.receive(60).empty());
}

TEST(OscFeedbackIntegrationTest, StatusAndSettingsValidation) {
    Show show;
    using Status = ExternalControl::FeedbackStatus;
    EXPECT_EQ(show.control.feedbackStatus(), Status::Off);
    show.control.updateFeedback(0.0);  // off: nothing
    EXPECT_EQ(show.control.osc().packetsSent(), 0u);

    show.control.setFeedback({true, "not.an.ip", 9000, false});
    EXPECT_EQ(show.control.feedbackStatus(), Status::InvalidSettings);
    EXPECT_FALSE(show.control.feedbackError().empty());
    show.control.setFeedback({true, "127.0.0.1", 0, false});
    EXPECT_EQ(show.control.feedbackStatus(), Status::InvalidSettings) << "a fixed host needs a port";
    show.control.setFeedback({true, "", 70000, false});
    EXPECT_EQ(show.control.feedbackStatus(), Status::InvalidSettings);
    show.control.setFeedback({true, "example.com", 9000, false});
    EXPECT_EQ(show.control.feedbackStatus(), Status::InvalidSettings) << "no DNS lookups";

    show.control.setFeedback({true, "127.0.0.1", 9, false});
    EXPECT_EQ(show.control.feedbackStatus(), Status::Sending);
    show.control.stopOsc();
    EXPECT_EQ(show.control.feedbackStatus(), Status::NeedsOscInput);
    EXPECT_FALSE(show.control.feedbackDestination().has_value());
    show.control.updateFeedback(10.0);
    EXPECT_EQ(show.control.osc().packetsSent(), 0u);
}

TEST(OscFeedbackIntegrationTest, RestartingOscSendsEverythingAgain) {
    Show show;
    Peer peer;
    show.control.setFeedback({true, "127.0.0.1", peer.port(), false});
    show.control.updateFeedback(0.0);
    EXPECT_EQ(peer.receive().size(), 5u);
    ASSERT_TRUE(show.control.startOsc("127.0.0.1", 0));
    show.control.updateFeedback(1.0);
    EXPECT_EQ(peer.receive().size(), 5u);
}

TEST(OscServerFeedbackTest, SendToNeedsARunningServerAndRecordsTheSender) {
    ControlEventQueue queue;
    OscServer server(queue);
    EXPECT_FALSE(server.sendTo({0x7F000001, 9}, {0, 0, 0, 0}));
    EXPECT_FALSE(server.lastSender().has_value());
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    EXPECT_FALSE(server.lastSender().has_value());

    Peer peer;
    peer.send(server.boundPort(), msg("/x", {f(1.0f)}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!server.lastSender() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(server.lastSender().has_value());
    EXPECT_EQ(server.lastSender()->ip, 0x7F000001u);
    EXPECT_EQ(server.lastSender()->port, peer.port());
    std::vector<ControlEvent> events;
    queue.drain(events, 10);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].oscSenderIp, 0x7F000001u);
    EXPECT_EQ(events[0].oscSenderPort, peer.port());

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(writeOscMessage(msg("/y", {f(2.0f)}), bytes));
    EXPECT_TRUE(server.sendTo(*server.lastSender(), bytes));
    EXPECT_EQ(server.packetsSent(), 1u);
    const auto back = peer.receive();
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].address, "/y");

    EXPECT_EQ(OscServer::toString({0xC0A80114, 9000}), "192.168.1.20:9000");
    EXPECT_EQ(OscServer::parseIpv4("192.168.1.20"), std::optional<uint32_t>(0xC0A80114u));
    EXPECT_FALSE(OscServer::parseIpv4("").has_value());
    EXPECT_FALSE(OscServer::parseIpv4("256.1.1.1").has_value());
    EXPECT_FALSE(OscServer::parseIpv4("localhost").has_value());
    server.stop();
    EXPECT_FALSE(server.sendTo({0x7F000001, static_cast<uint16_t>(peer.port())}, bytes));
}
