#include <gtest/gtest.h>
#include "audio/AudioDevice.hpp"

namespace quadmapper {
namespace test {

class AudioDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        device = std::make_unique<AudioDevice>();
    }

    void TearDown() override {
        if (device) {
            device->shutdown();
        }
    }

    std::unique_ptr<AudioDevice> device;
};

// --- Construction Tests ---

TEST_F(AudioDeviceTest, DefaultConstruction) {
    EXPECT_FALSE(device->isInitialized());
}

// --- Initialization Tests ---
// Note: These tests may pass/fail depending on audio hardware availability

TEST_F(AudioDeviceTest, Initialize) {
    bool result = device->initialize();
    // May fail on systems without audio hardware
    if (result) {
        EXPECT_TRUE(device->isInitialized());
    }
}

TEST_F(AudioDeviceTest, ShutdownSafe) {
    device->shutdown();
    EXPECT_FALSE(device->isInitialized());
}

TEST_F(AudioDeviceTest, DoubleInitialize) {
    if (device->initialize()) {
        bool result = device->initialize();
        EXPECT_TRUE(result);  // Second init should succeed
    }
}

TEST_F(AudioDeviceTest, DoubleShutdown) {
    if (device->initialize()) {
        device->shutdown();
        device->shutdown();  // Should not crash
        EXPECT_FALSE(device->isInitialized());
    }
}

// --- Device Enumeration Tests (QM-031) ---

TEST_F(AudioDeviceTest, EnumerateDevicesEmpty) {
    // Without initialization, should return empty list
    auto devices = device->enumerateDevices();
    EXPECT_TRUE(devices.empty());
}

TEST_F(AudioDeviceTest, EnumerateDevices) {
    if (device->initialize()) {
        auto devices = device->enumerateDevices();
        // May or may not have devices depending on system
        // At minimum, we expect the call to succeed
        for (const auto& d : devices) {
            EXPECT_FALSE(d.id.empty());
            EXPECT_FALSE(d.name.empty());
            EXPECT_TRUE(d.isPlayback);
        }
    }
}

TEST_F(AudioDeviceTest, RefreshDevices) {
    if (device->initialize()) {
        auto before = device->enumerateDevices();
        device->refreshDevices();
        auto after = device->enumerateDevices();
        // List may differ if hardware changed, but call should succeed
    }
}

TEST_F(AudioDeviceTest, GetDefaultDevice) {
    if (device->initialize()) {
        auto devices = device->enumerateDevices();
        if (!devices.empty()) {
            auto defaultDevice = device->getDefaultDevice();
            // May or may not have a default
            if (defaultDevice.isValid()) {
                EXPECT_FALSE(defaultDevice.name.empty());
            }
        }
    }
}

TEST_F(AudioDeviceTest, GetSelectedDevice) {
    if (device->initialize()) {
        auto selected = device->getSelectedDevice();
        // Should have a selected device if any devices exist
        auto devices = device->enumerateDevices();
        if (!devices.empty()) {
            // Selected device should be valid
            // (defaults to first device if none selected)
        }
    }
}

// --- Device Selection Tests (QM-031) ---

TEST_F(AudioDeviceTest, SelectDevice) {
    if (device->initialize()) {
        auto devices = device->enumerateDevices();
        if (!devices.empty()) {
            bool result = device->selectDevice(devices[0].id);
            EXPECT_TRUE(result);

            auto selected = device->getSelectedDevice();
            EXPECT_EQ(selected.id, devices[0].id);
        }
    }
}

TEST_F(AudioDeviceTest, SelectNonexistentDevice) {
    if (device->initialize()) {
        bool result = device->selectDevice("nonexistent_device_id");
        EXPECT_FALSE(result);
    }
}

TEST_F(AudioDeviceTest, SelectDefaultDevice) {
    if (device->initialize()) {
        auto devices = device->enumerateDevices();
        if (!devices.empty()) {
            bool result = device->selectDefaultDevice();
            EXPECT_TRUE(result);
        }
    }
}

TEST_F(AudioDeviceTest, SelectDeviceEmptyId) {
    if (device->initialize()) {
        auto devices = device->enumerateDevices();
        if (!devices.empty()) {
            // Empty ID should select default
            // Result depends on default device availability
            (void)device->selectDevice("");
        }
    }
}

// --- Sample Rate Tests ---

TEST_F(AudioDeviceTest, DefaultSampleRate) {
    EXPECT_EQ(device->getSampleRate(), AudioDevice::DEFAULT_SAMPLE_RATE);
}

TEST_F(AudioDeviceTest, SetSampleRate) {
    bool result = device->setSampleRate(44100);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getSampleRate(), 44100);
}

TEST_F(AudioDeviceTest, SetSampleRate48000) {
    bool result = device->setSampleRate(48000);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getSampleRate(), 48000);
}

TEST_F(AudioDeviceTest, SetSampleRateTooLow) {
    bool result = device->setSampleRate(100);
    EXPECT_FALSE(result);
}

TEST_F(AudioDeviceTest, SetSampleRateTooHigh) {
    bool result = device->setSampleRate(1000000);
    EXPECT_FALSE(result);
}

// --- Channel Count Tests ---

TEST_F(AudioDeviceTest, DefaultChannels) {
    EXPECT_EQ(device->getChannels(), AudioDevice::DEFAULT_CHANNELS);
}

TEST_F(AudioDeviceTest, SetChannelsMono) {
    bool result = device->setChannels(1);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getChannels(), 1);
}

TEST_F(AudioDeviceTest, SetChannelsStereo) {
    bool result = device->setChannels(2);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getChannels(), 2);
}

TEST_F(AudioDeviceTest, SetChannelsSurround) {
    bool result = device->setChannels(6);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getChannels(), 6);
}

TEST_F(AudioDeviceTest, SetChannelsZero) {
    bool result = device->setChannels(0);
    EXPECT_FALSE(result);
}

TEST_F(AudioDeviceTest, SetChannelsTooMany) {
    bool result = device->setChannels(100);
    EXPECT_FALSE(result);
}

// --- Buffer Size Tests ---

TEST_F(AudioDeviceTest, DefaultBufferSize) {
    EXPECT_EQ(device->getBufferSizeFrames(), AudioDevice::DEFAULT_BUFFER_FRAMES);
}

TEST_F(AudioDeviceTest, SetBufferSize) {
    bool result = device->setBufferSizeFrames(1024);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getBufferSizeFrames(), 1024u);
}

TEST_F(AudioDeviceTest, SetBufferSizeLow) {
    bool result = device->setBufferSizeFrames(64);
    EXPECT_TRUE(result);
    EXPECT_EQ(device->getBufferSizeFrames(), 64u);
}

TEST_F(AudioDeviceTest, SetBufferSizeTooLow) {
    bool result = device->setBufferSizeFrames(32);
    EXPECT_FALSE(result);
}

TEST_F(AudioDeviceTest, SetBufferSizeTooHigh) {
    bool result = device->setBufferSizeFrames(100000);
    EXPECT_FALSE(result);
}

// --- Constants Tests ---

TEST_F(AudioDeviceTest, Constants) {
    EXPECT_EQ(AudioDevice::DEFAULT_SAMPLE_RATE, 48000);
    EXPECT_EQ(AudioDevice::DEFAULT_CHANNELS, 2);
    EXPECT_EQ(AudioDevice::DEFAULT_BUFFER_FRAMES, 512u);
}

// --- AudioDeviceInfo Tests ---

TEST(AudioDeviceInfoTest, DefaultIsInvalid) {
    AudioDeviceInfo info;
    EXPECT_FALSE(info.isValid());
    EXPECT_TRUE(info.id.empty());
    EXPECT_TRUE(info.name.empty());
    EXPECT_FALSE(info.isDefault);
    EXPECT_TRUE(info.isPlayback);
}

TEST(AudioDeviceInfoTest, ValidWithIdAndName) {
    AudioDeviceInfo info;
    info.id = "device_1";
    info.name = "Test Device";

    EXPECT_TRUE(info.isValid());
}

TEST(AudioDeviceInfoTest, InvalidWithoutId) {
    AudioDeviceInfo info;
    info.name = "Test Device";

    EXPECT_FALSE(info.isValid());
}

TEST(AudioDeviceInfoTest, InvalidWithoutName) {
    AudioDeviceInfo info;
    info.id = "device_1";

    EXPECT_FALSE(info.isValid());
}

// --- Move Semantics Tests ---

TEST_F(AudioDeviceTest, MoveConstruction) {
    // Create a new device for this test to avoid using the fixture device
    auto testDevice = std::make_unique<AudioDevice>();
    testDevice->setSampleRate(44100);
    testDevice->setChannels(1);
    testDevice->setBufferSizeFrames(256);

    AudioDevice moved(std::move(*testDevice));

    EXPECT_EQ(moved.getSampleRate(), 44100);
    EXPECT_EQ(moved.getChannels(), 1);
    EXPECT_EQ(moved.getBufferSizeFrames(), 256u);

    // Don't use testDevice after move - it's in moved-from state
}

} // namespace test
} // namespace quadmapper
