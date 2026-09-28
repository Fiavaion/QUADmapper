#include "control/MidiDecode.hpp"
#include "control/MidiInput.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace quadmapper::control;

namespace {
// WinMM MIM_DATA dwParam1 layout: status | data1 << 8 | data2 << 16.
uint32_t shortMessage(uint8_t status, uint8_t data1, uint8_t data2 = 0) {
    return static_cast<uint32_t>(status) | (static_cast<uint32_t>(data1) << 8) |
           (static_cast<uint32_t>(data2) << 16);
}
}  // namespace

TEST(MidiDecodeTest, NoteOnWithChannelAndVelocity) {
    auto m = decodeMidiShortMessage(shortMessage(0x93, 60, 100));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->type, MidiType::NoteOn);
    EXPECT_EQ(m->channel, 4);  // low nibble 3 = channel 4
    EXPECT_EQ(m->number, 60);
    EXPECT_EQ(m->value, 100);
}

TEST(MidiDecodeTest, NoteOnVelocityZeroIsNoteOff) {
    auto m = decodeMidiShortMessage(shortMessage(0x90, 60, 0));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->type, MidiType::NoteOff);
    EXPECT_EQ(m->value, 0);
}

TEST(MidiDecodeTest, NoteOff) {
    auto m = decodeMidiShortMessage(shortMessage(0x8F, 61, 64));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->type, MidiType::NoteOff);
    EXPECT_EQ(m->channel, 16);
    EXPECT_EQ(m->number, 61);
}

TEST(MidiDecodeTest, ControlChangeAndProgramChange) {
    auto cc = decodeMidiShortMessage(shortMessage(0xB0, 7, 127));
    ASSERT_TRUE(cc);
    EXPECT_EQ(cc->type, MidiType::ControlChange);
    EXPECT_EQ(cc->channel, 1);
    EXPECT_EQ(cc->number, 7);
    EXPECT_EQ(cc->value, 127);

    auto pc = decodeMidiShortMessage(shortMessage(0xC2, 5, 99));
    ASSERT_TRUE(pc);
    EXPECT_EQ(pc->type, MidiType::ProgramChange);
    EXPECT_EQ(pc->channel, 3);
    EXPECT_EQ(pc->number, 5);
    EXPECT_EQ(pc->value, 0);  // program change has one data byte
}

TEST(MidiDecodeTest, MessagesWithoutStatusByteAreRejected) {
    // WinMM expands running status, so a data byte in the status position is malformed.
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0x3C, 0x64, 0)));
    EXPECT_FALSE(decodeMidiShortMessage(0));
}

TEST(MidiDecodeTest, IgnoredMessageTypes) {
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0xA0, 60, 10)));  // poly aftertouch
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0xD0, 10)));      // channel pressure
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0xE0, 0, 64)));   // pitch bend
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0xF8, 0)));       // clock
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0xF0, 0)));       // SysEx start
    EXPECT_FALSE(decodeMidiShortMessage(shortMessage(0xFE, 0)));       // active sensing
}

TEST(MidiDecodeTest, DataBytesAreMaskedTo7Bits) {
    auto m = decodeMidiShortMessage(shortMessage(0xB0, 0xFF, 0xFF) | 0xFF000000u);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->number, 127);
    EXPECT_EQ(m->value, 127);
}

TEST(MidiDecodeTest, EveryShortMessageDecodesSafely) {
    // Exhaustive over status x data1 with a few data2 values: never out of range.
    for (uint32_t status = 0; status < 256; ++status) {
        for (uint32_t d1 = 0; d1 < 256; ++d1) {
            for (uint32_t d2 : {0u, 1u, 63u, 64u, 127u, 128u, 255u}) {
                auto m = decodeMidiShortMessage(status | (d1 << 8) | (d2 << 16));
                if (!m) continue;
                EXPECT_GE(m->channel, 1);
                EXPECT_LE(m->channel, 16);
                EXPECT_LE(m->number, 127);
                EXPECT_LE(m->value, 127);
                if (m->type == MidiType::NoteOn) EXPECT_GT(m->value, 0);
            }
        }
    }
}

// ---- WinMM device handling (no hardware required) -------------------------------

TEST(MidiInputTest, ListingDevicesWorksWithOrWithoutHardware) {
    const auto devices = MidiInput::listDevices();
    for (const auto& name : devices) EXPECT_FALSE(name.empty());
    SUCCEED() << devices.size() << " MIDI input device(s) present";
}

TEST(MidiInputTest, OpeningAnAbsentDeviceFailsCleanly) {
    ControlEventQueue queue;
    MidiInput midi(queue);
    auto result = midi.open("QUADmapper test device that does not exist");
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("not connected"), std::string::npos);
    EXPECT_FALSE(midi.isOpen("QUADmapper test device that does not exist"));
    EXPECT_TRUE(midi.openDeviceNames().empty());
    midi.close("QUADmapper test device that does not exist");  // no-op
    midi.closeAll();
    midi.closeAll();
}

TEST(MidiInputTest, OpenAndCloseEveryPresentDevice) {
    // Exercises the real WinMM open/start/stop/close path on whatever is installed.
    ControlEventQueue queue;
    MidiInput midi(queue);
    const auto devices = MidiInput::listDevices();
    if (devices.empty()) GTEST_SKIP() << "no MIDI input device on this machine";
    for (const auto& name : devices) {
        auto result = midi.open(name);
        if (!result) continue;  // a device another program holds exclusively
        EXPECT_TRUE(midi.isOpen(name));
        midi.close(name);
        EXPECT_FALSE(midi.isOpen(name));
    }
}

TEST(MidiInputTest, CloseMissingClosesOnlyVanishedDevices) {
    ControlEventQueue queue;
    MidiInput midi(queue);
    const auto devices = MidiInput::listDevices();
    if (devices.empty()) GTEST_SKIP() << "no MIDI input device on this machine";
    if (!midi.open(devices.front())) GTEST_SKIP() << "device busy";
    EXPECT_TRUE(midi.closeMissing(devices).empty());
    EXPECT_TRUE(midi.isOpen(devices.front()));
    const auto closed = midi.closeMissing({});  // as if it was unplugged
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_FALSE(midi.isOpen(devices.front()));
}
