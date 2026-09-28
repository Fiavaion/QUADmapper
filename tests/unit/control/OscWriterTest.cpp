#include "control/OscParser.hpp"
#include "control/OscWriter.hpp"

#include "OscTestPackets.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace quadmapper::control;
using quadmapper::control::test::OscPacket;

namespace {

OscArg i(int32_t v) { OscArg a; a.type = OscArg::Type::Int; a.tag = 'i'; a.i = v; return a; }
OscArg f(float v) { OscArg a; a.type = OscArg::Type::Float; a.tag = 'f'; a.f = v; return a; }
OscArg s(const std::string& v) { OscArg a; a.type = OscArg::Type::String; a.tag = 's'; a.s = v; return a; }
OscArg T() { OscArg a; a.type = OscArg::Type::True; a.tag = 'T'; return a; }
OscArg F() { OscArg a; a.type = OscArg::Type::False; a.tag = 'F'; return a; }

OscMessage msg(const std::string& address, std::vector<OscArg> args = {}) {
    OscMessage m;
    m.address = address;
    m.args = std::move(args);
    return m;
}

std::vector<uint8_t> write(const OscMessage& m) {
    std::vector<uint8_t> out;
    EXPECT_TRUE(writeOscMessage(m, out));
    return out;
}

void expectSameArgs(const OscMessage& a, const OscMessage& b) {
    EXPECT_EQ(a.address, b.address);
    ASSERT_EQ(a.args.size(), b.args.size());
    for (size_t k = 0; k < a.args.size(); ++k) {
        SCOPED_TRACE(k);
        EXPECT_EQ(a.args[k].type, b.args[k].type);
        EXPECT_EQ(a.args[k].tag, b.args[k].tag);
        EXPECT_EQ(a.args[k].i, b.args[k].i);
        EXPECT_EQ(a.args[k].s, b.args[k].s);
        if (std::isnan(a.args[k].f)) EXPECT_TRUE(std::isnan(b.args[k].f));
        else EXPECT_EQ(a.args[k].f, b.args[k].f);
    }
}

}  // namespace

// ---- Byte-exact against published packets ---------------------------------------

// The two example messages of the OSC 1.0 specification, byte for byte.
TEST(OscWriterTest, WritesTheSpecificationsFirstExampleExactly) {
    const std::vector<uint8_t> expected = {
        0x2f, 0x6f, 0x73, 0x63, 0x69, 0x6c, 0x6c, 0x61, 0x74, 0x6f, 0x72, 0x2f,  // /oscillator/
        0x34, 0x2f, 0x66, 0x72, 0x65, 0x71, 0x75, 0x65, 0x6e, 0x63, 0x79, 0x00,  // 4/frequency\0
        0x2c, 0x66, 0x00, 0x00,                                                  // ,f
        0x43, 0xdc, 0x00, 0x00};                                                 // 440.0
    EXPECT_EQ(write(msg("/oscillator/4/frequency", {f(440.0f)})), expected);
}

TEST(OscWriterTest, WritesTheSpecificationsSecondExampleExactly) {
    const std::vector<uint8_t> expected = {
        0x2f, 0x66, 0x6f, 0x6f, 0x00, 0x00, 0x00, 0x00,  // /foo + 4 NULs (length is a multiple of 4)
        0x2c, 0x69, 0x69, 0x73, 0x66, 0x66, 0x00, 0x00,  // ,iisff
        0x00, 0x00, 0x03, 0xe8,                          // 1000
        0xff, 0xff, 0xff, 0xff,                          // -1
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00,  // hello
        0x3f, 0x9d, 0xf3, 0xb6,                          // 1.234
        0x40, 0xb5, 0xb2, 0x2d};                         // 5.678
    EXPECT_EQ(write(msg("/foo", {i(1000), i(-1), s("hello"), f(1.234f), f(5.678f)})), expected);
}

TEST(OscWriterTest, WritesTrueFalseAndNoArgumentsExactly) {
    EXPECT_EQ(write(msg("/a", {T(), F()})),
              (std::vector<uint8_t>{'/', 'a', 0, 0, ',', 'T', 'F', 0}));
    // No arguments still carries the "," type-tag string (OSC 1.0 senders should).
    EXPECT_EQ(write(msg("/abc")), (std::vector<uint8_t>{'/', 'a', 'b', 'c', 0, 0, 0, 0, ',', 0, 0, 0}));
    // Empty string argument: one NUL padded to 4.
    EXPECT_EQ(write(msg("/ab", {s("")})),
              (std::vector<uint8_t>{'/', 'a', 'b', 0, ',', 's', 0, 0, 0, 0, 0, 0}));
}

// The same bytes the test packet builder (used against the parser) produces.
TEST(OscWriterTest, MatchesTheTestPacketBuilder) {
    OscPacket p;
    p.string("/qm/quad/Left/opacity").string(",fis").float32(0.25f).int32(-7).string("abc");
    EXPECT_EQ(write(msg("/qm/quad/Left/opacity", {f(0.25f), i(-7), s("abc")})), p.bytes());
}

TEST(OscWriterTest, AppendsAfterExistingBytesWithPaddingCountedFromThePacket) {
    std::vector<uint8_t> out = {0xAA, 0xBB, 0xCC};  // unaligned prefix
    ASSERT_TRUE(writeOscMessage(msg("/x", {s("hi")}), out));
    const std::vector<uint8_t> packet(out.begin() + 3, out.end());
    EXPECT_EQ(packet, write(msg("/x", {s("hi")})));
    EXPECT_EQ(packet.size() % 4, 0u);
}

// ---- Rejections --------------------------------------------------------------------

TEST(OscWriterTest, RejectsWhatCannotBeWrittenAndLeavesOutputUnchanged) {
    OscArg unsupported;
    unsupported.type = OscArg::Type::Unsupported;
    unsupported.tag = 'b';
    const std::vector<OscMessage> bad = {
        msg(""), msg("noslash"), msg(std::string("/a\0b", 4)),
        msg("/ok", {s(std::string("x\0y", 3))}), msg("/ok", {f(1.0f), unsupported})};
    for (const auto& m : bad) {
        SCOPED_TRACE(m.address);
        std::vector<uint8_t> out = {1, 2, 3, 4};
        EXPECT_FALSE(writeOscMessage(m, out));
        EXPECT_EQ(out, (std::vector<uint8_t>{1, 2, 3, 4}));
    }
}

// ---- Round trip through the parser -------------------------------------------------

TEST(OscWriterTest, RoundTripsThroughTheParser) {
    const std::vector<OscMessage> cases = {
        msg("/qm/blackout"),
        msg("/qm/blackout", {T()}),
        msg("/qm/blackout", {F()}),
        msg("/qm/scene/active", {i(3)}),
        msg("/qm/quad/11111111-aaaa-bbbb-cccc-000000000001/opacity", {f(0.5f)}),
        msg("/a", {i(std::numeric_limits<int32_t>::min()), i(std::numeric_limits<int32_t>::max()), i(0)}),
        msg("/abc", {f(-0.0f), f(std::numeric_limits<float>::infinity()),
                     f(std::numeric_limits<float>::denorm_min()), f(std::numeric_limits<float>::quiet_NaN())}),
        msg("/abcd", {s("a"), s("ab"), s("abc"), s("abcd"), s("")}),
        msg("/mixed", {T(), i(1), F(), f(2.5f), s("x y"), T()}),
    };
    for (const auto& original : cases) {
        SCOPED_TRACE(original.address);
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(writeOscMessage(original, bytes));
        EXPECT_EQ(bytes.size() % 4, 0u);
        std::vector<OscMessage> parsed;
        ASSERT_TRUE(parseOscPacket(bytes.data(), bytes.size(), parsed));
        ASSERT_EQ(parsed.size(), 1u);
        expectSameArgs(original, parsed[0]);
    }
}

// Random messages: write -> parse -> write is the identity on the bytes.
TEST(OscWriterTest, RandomMessagesRoundTripByteExact) {
    std::mt19937 rng(48);
    auto randomText = [&](size_t maxLen) {
        std::string text;
        const size_t n = rng() % (maxLen + 1);
        for (size_t k = 0; k < n; ++k) text.push_back(static_cast<char>(1 + rng() % 126));
        return text;
    };
    for (int round = 0; round < 2000; ++round) {
        OscMessage m;
        m.address = "/" + randomText(20);
        const size_t argc = rng() % 8;
        for (size_t k = 0; k < argc; ++k) {
            switch (rng() % 5) {
                case 0: m.args.push_back(i(static_cast<int32_t>(rng()))); break;
                case 1: {
                    uint32_t bits = rng();
                    float v;
                    std::memcpy(&v, &bits, sizeof(v));
                    m.args.push_back(f(v));
                    break;
                }
                case 2: m.args.push_back(s(randomText(13))); break;
                case 3: m.args.push_back(T()); break;
                default: m.args.push_back(F()); break;
            }
        }
        std::vector<uint8_t> first;
        ASSERT_TRUE(writeOscMessage(m, first));
        std::vector<OscMessage> parsed;
        ASSERT_TRUE(parseOscPacket(first.data(), first.size(), parsed)) << "round " << round;
        ASSERT_EQ(parsed.size(), 1u);
        std::vector<uint8_t> second;
        ASSERT_TRUE(writeOscMessage(parsed[0], second));
        ASSERT_EQ(first, second) << "round " << round;
    }
}
