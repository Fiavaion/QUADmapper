#include "control/OscParser.hpp"

#include "OscTestPackets.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <random>

using namespace quadmapper::control;
using quadmapper::control::test::OscPacket;
using quadmapper::control::test::oscMessage;

namespace {

bool parse(const std::vector<uint8_t>& bytes, std::vector<OscMessage>& out) {
    return parseOscPacket(bytes.data(), bytes.size(), out);
}

// Places data so that its last byte is the last readable byte before a no-access
// page: any read past the end is an access violation, not a silent overrun.
class GuardedBuffer {
public:
    GuardedBuffer() {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, page_ * 2, MEM_RESERVE | MEM_COMMIT,
                                                   PAGE_READWRITE));
        DWORD old = 0;
        VirtualProtect(base_ + page_, page_, PAGE_NOACCESS, &old);
    }
    ~GuardedBuffer() { VirtualFree(base_, 0, MEM_RELEASE); }
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;

    bool parse(const uint8_t* data, size_t size, std::vector<OscMessage>& out) {
        if (size > page_) size = page_;
        uint8_t* at = base_ + page_ - size;
        if (size) std::memcpy(at, data, size);
        return parseOscPacket(at, size, out);
    }

private:
    uint8_t* base_ = nullptr;
    size_t page_ = 4096;
};

}  // namespace

// ---- Exact decodes of known byte sequences ------------------------------------------

TEST(OscParserTest, DecodesSpecExampleFloatMessage) {
    // "/oscillator/4/frequency" ,f 440.0 - the OSC 1.0 specification's example.
    const std::vector<uint8_t> bytes = {
        0x2f, 0x6f, 0x73, 0x63, 0x69, 0x6c, 0x6c, 0x61, 0x74, 0x6f, 0x72, 0x2f,
        0x34, 0x2f, 0x66, 0x72, 0x65, 0x71, 0x75, 0x65, 0x6e, 0x63, 0x79, 0x00,
        0x2c, 0x66, 0x00, 0x00, 0x43, 0xdc, 0x00, 0x00};
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(bytes, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].address, "/oscillator/4/frequency");
    ASSERT_EQ(out[0].args.size(), 1u);
    EXPECT_EQ(out[0].args[0].type, OscArg::Type::Float);
    EXPECT_FLOAT_EQ(out[0].args[0].f, 440.0f);
}

TEST(OscParserTest, DecodesSpecExampleMixedMessage) {
    // "/foo" ,iisff 1000 -1 "hello" 1.234 5.678 - the specification's second example.
    const std::vector<uint8_t> bytes = {
        0x2f, 0x66, 0x6f, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x69, 0x69, 0x73,
        0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x03, 0xe8, 0xff, 0xff, 0xff, 0xff,
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00, 0x3f, 0x9d, 0xf3, 0xb6,
        0x40, 0xb5, 0xb2, 0x2d};
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(bytes, out));
    ASSERT_EQ(out.size(), 1u);
    const auto& args = out[0].args;
    EXPECT_EQ(out[0].address, "/foo");
    ASSERT_EQ(args.size(), 5u);
    EXPECT_EQ(args[0].type, OscArg::Type::Int);
    EXPECT_EQ(args[0].i, 1000);
    EXPECT_EQ(args[1].i, -1);
    EXPECT_EQ(args[2].type, OscArg::Type::String);
    EXPECT_EQ(args[2].s, "hello");
    EXPECT_NEAR(args[3].f, 1.234f, 1e-6f);
    EXPECT_NEAR(args[4].f, 5.678f, 1e-6f);
}

TEST(OscParserTest, DecodesTrueFalseWithoutData) {
    auto p = oscMessage("/qm/blackout");
    p.string(",TF");
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(p.bytes(), out));
    ASSERT_EQ(out[0].args.size(), 2u);
    EXPECT_EQ(out[0].args[0].type, OscArg::Type::True);
    EXPECT_EQ(out[0].args[1].type, OscArg::Type::False);
}

TEST(OscParserTest, MessageWithoutTypeTagsHasNoArguments) {
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(oscMessage("/qm/blackout").bytes(), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].args.empty());
}

TEST(OscParserTest, EmptyTypeTagStringHasNoArguments) {
    auto p = oscMessage("/a");
    p.string(",");
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(p.bytes(), out));
    EXPECT_TRUE(out[0].args.empty());
}

TEST(OscParserTest, AddressExactlyFourBytesGetsAFullPadWord) {
    // "/abc" is 4 chars, so its NUL needs a whole extra word: 8 bytes total.
    auto p = oscMessage("/abc");
    ASSERT_EQ(p.bytes().size(), 8u);
    p.string(",i").int32(7);
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(p.bytes(), out));
    EXPECT_EQ(out[0].address, "/abc");
    EXPECT_EQ(out[0].args[0].i, 7);
}

TEST(OscParserTest, SkipsKnownButUnsupportedTypesKeepingPositions) {
    auto p = oscMessage("/x");
    p.string(",bhdiNS[f]");
    p.int32(5).raw({1, 2, 3, 4, 5, 0, 0, 0});  // blob of 5 + 3 pad
    p.int32(0).int32(9);                        // h
    p.int32(0).int32(0);                        // d
    p.int32(42);                                // i
    p.string("sym");                            // S
    p.float32(0.5f);                            // f (inside [ ])
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(p.bytes(), out));
    const auto& args = out[0].args;
    ASSERT_EQ(args.size(), 7u);  // b h d i N S f; [ ] are not arguments
    EXPECT_EQ(args[0].type, OscArg::Type::Unsupported);
    EXPECT_EQ(args[0].tag, 'b');
    EXPECT_EQ(args[3].type, OscArg::Type::Int);
    EXPECT_EQ(args[3].i, 42);
    EXPECT_EQ(args[4].tag, 'N');
    EXPECT_EQ(args[5].tag, 'S');
    EXPECT_EQ(args[6].type, OscArg::Type::Float);
    EXPECT_FLOAT_EQ(args[6].f, 0.5f);
}

TEST(OscParserTest, UnknownTypeTagRejectsThePacket) {
    auto p = oscMessage("/x");
    p.string(",iQ").int32(1).int32(2);
    std::vector<OscMessage> out;
    EXPECT_FALSE(parse(p.bytes(), out));
    EXPECT_TRUE(out.empty());
}

TEST(OscParserTest, BundleWithNestedBundle) {
    auto a = oscMessage("/a");
    a.string(",i").int32(1);
    auto b = oscMessage("/b");
    b.string(",f").float32(2.0f);
    OscPacket inner;
    inner.bundleHeader().element(b);
    OscPacket outer;
    outer.bundleHeader().element(a).element(inner);

    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(outer.bytes(), out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].address, "/a");
    EXPECT_EQ(out[0].args[0].i, 1);
    EXPECT_EQ(out[1].address, "/b");
    EXPECT_FLOAT_EQ(out[1].args[0].f, 2.0f);
}

TEST(OscParserTest, EmptyBundleIsValid) {
    OscPacket p;
    p.bundleHeader();
    std::vector<OscMessage> out;
    EXPECT_TRUE(parse(p.bytes(), out));
    EXPECT_TRUE(out.empty());
}

TEST(OscParserTest, BundleNestingDeeperThanTheLimitIsRejected) {
    auto leaf = oscMessage("/deep");
    OscPacket current = leaf;
    for (int depth = 0; depth < kMaxBundleDepth; ++depth) {
        OscPacket wrap;
        wrap.bundleHeader().element(current);
        current = wrap;
    }
    std::vector<OscMessage> out;
    EXPECT_TRUE(parse(current.bytes(), out)) << "exactly the limit is accepted";

    OscPacket tooDeep;
    tooDeep.bundleHeader().element(current);
    out.clear();
    EXPECT_FALSE(parse(tooDeep.bytes(), out));
}

TEST(OscParserTest, BundleElementSizeMustFit) {
    auto a = oscMessage("/a");
    OscPacket p;
    p.bundleHeader().int32(static_cast<int32_t>(a.bytes().size() + 4)).raw(a.bytes());
    std::vector<OscMessage> out;
    EXPECT_FALSE(parse(p.bytes(), out));

    OscPacket negative;
    negative.bundleHeader().int32(-4).raw(a.bytes());
    EXPECT_FALSE(parse(negative.bytes(), out));

    OscPacket zero;
    zero.bundleHeader().int32(0);
    EXPECT_FALSE(parse(zero.bytes(), out));
}

TEST(OscParserTest, OneBadElementRejectsTheWholeBundle) {
    auto good = oscMessage("/good");
    OscPacket bad;
    bad.raw({'x', 0, 0, 0});  // not '/' and not "#bundle"
    OscPacket p;
    p.bundleHeader().element(good).element(bad);
    std::vector<OscMessage> out;
    EXPECT_FALSE(parse(p.bytes(), out));
    EXPECT_TRUE(out.empty()) << "all-or-nothing";
}

TEST(OscParserTest, RejectsMisalignedAndMalformedPackets) {
    std::vector<OscMessage> out;
    EXPECT_FALSE(parseOscPacket(nullptr, 0, out));
    EXPECT_FALSE(parse({}, out));
    EXPECT_FALSE(parse({'/', 'a', 0}, out));                      // not a multiple of 4
    EXPECT_FALSE(parse({'/', 'a', 'b', 'c'}, out));               // no NUL
    EXPECT_FALSE(parse({'a', 'b', 'c', 0}, out));                 // no leading '/'
    EXPECT_FALSE(parse({'/', 'a', 0, 0, 'i', 0, 0, 0}, out));     // tags without ','
    EXPECT_FALSE(parse({'#', 'b', 'u', 'n', 'd', 'l', 'e', 0}, out));  // bundle without time tag

    auto truncatedInt = oscMessage("/a");
    truncatedInt.string(",i");  // the int is missing
    EXPECT_FALSE(parse(truncatedInt.bytes(), out));

    auto hugeBlob = oscMessage("/a");
    hugeBlob.string(",b").int32(0x7FFFFFF0);
    EXPECT_FALSE(parse(hugeBlob.bytes(), out));

    auto negativeBlob = oscMessage("/a");
    negativeBlob.string(",b").int32(-1);
    EXPECT_FALSE(parse(negativeBlob.bytes(), out));

    auto unterminatedString = oscMessage("/a");
    unterminatedString.string(",s").raw({'a', 'b', 'c', 'd'});
    EXPECT_FALSE(parse(unterminatedString.bytes(), out));
    EXPECT_TRUE(out.empty());
}

TEST(OscParserTest, CapsMessagesPerPacket) {
    auto m = oscMessage("/m");
    OscPacket p;
    p.bundleHeader();
    for (size_t i = 0; i < kMaxMessagesPerPacket; ++i) p.element(m);
    std::vector<OscMessage> out;
    ASSERT_TRUE(parse(p.bytes(), out));
    EXPECT_EQ(out.size(), kMaxMessagesPerPacket);

    p.element(m);
    out.clear();
    EXPECT_FALSE(parse(p.bytes(), out));
}

TEST(OscParserTest, AppendsToExistingOutput) {
    std::vector<OscMessage> out(1);
    ASSERT_TRUE(parse(oscMessage("/a").bytes(), out));
    EXPECT_EQ(out.size(), 2u);
}

// ---- Fuzz: never crash, never read out of bounds -------------------------------------

namespace {

std::vector<std::vector<uint8_t>> validCorpus() {
    std::vector<std::vector<uint8_t>> corpus;
    auto a = oscMessage("/qm/quad/Left/opacity");
    a.string(",f").float32(0.25f);
    corpus.push_back(a.bytes());
    auto b = oscMessage("/qm/scene/take");
    b.string(",i").int32(2);
    corpus.push_back(b.bytes());
    auto c = oscMessage("/x");
    c.string(",sbTFhS").string("text").int32(3).raw({1, 2, 3, 0}).int32(0).int32(1).string("sym");
    corpus.push_back(c.bytes());
    OscPacket inner;
    inner.bundleHeader().element(a).element(b);
    OscPacket outer;
    outer.bundleHeader().element(inner).element(c);
    corpus.push_back(outer.bytes());
    return corpus;
}

}  // namespace

TEST(OscParserFuzzTest, RandomBytesNeverCrashOrOverrun) {
    GuardedBuffer guard;
    std::mt19937 rng(0x05C1u);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> length(0, 96);
    std::vector<uint8_t> data;
    std::vector<OscMessage> out;
    size_t accepted = 0;
    for (int iteration = 0; iteration < 200000; ++iteration) {
        data.resize(static_cast<size_t>(length(rng)) * 4 + static_cast<size_t>(iteration % 4 == 0 ? 0 : iteration % 4));
        for (auto& b : data) b = static_cast<uint8_t>(byte(rng));
        // Steer a share of inputs into the message and bundle paths.
        if (!data.empty() && iteration % 3 == 0) data[0] = '/';
        if (data.size() >= 16 && iteration % 5 == 0) std::memcpy(data.data(), "#bundle", 8);
        out.clear();
        if (guard.parse(data.data(), data.size(), out)) ++accepted;
    }
    SUCCEED() << accepted << " random packets happened to be valid";
}

TEST(OscParserFuzzTest, TruncationsOfValidPacketsNeverCrashOrOverrun) {
    GuardedBuffer guard;
    std::vector<OscMessage> out;
    for (const auto& packet : validCorpus()) {
        out.clear();
        ASSERT_TRUE(guard.parse(packet.data(), packet.size(), out));
        for (size_t cut = 0; cut < packet.size(); ++cut) {
            out.clear();
            guard.parse(packet.data(), cut, out);  // must not crash; result may be either
        }
    }
}

TEST(OscParserFuzzTest, MutationsOfValidPacketsNeverCrashOrOverrun) {
    GuardedBuffer guard;
    std::mt19937 rng(0xB0B5u);
    const auto corpus = validCorpus();
    std::vector<OscMessage> out;
    for (int iteration = 0; iteration < 150000; ++iteration) {
        std::vector<uint8_t> data = corpus[static_cast<size_t>(iteration) % corpus.size()];
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) {
            const size_t at = rng() % data.size();
            switch (rng() % 3) {
                case 0: data[at] = static_cast<uint8_t>(rng()); break;          // random byte
                case 1: data[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;  // bit flip
                case 2: data[at] = static_cast<uint8_t>(rng() % 2 ? 0xFF : 0x7F); break;  // big sizes
            }
        }
        if (iteration % 7 == 0) data.resize(rng() % (data.size() + 1));
        out.clear();
        guard.parse(data.data(), data.size(), out);
    }
}
