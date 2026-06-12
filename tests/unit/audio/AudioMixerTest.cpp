#include <gtest/gtest.h>
#include "audio/AudioMixer.hpp"
#include "audio/AudioSource.hpp"
#include "audio/AudioBuffer.hpp"
#include <cmath>
#include <thread>
#include <chrono>

namespace quadmapper {
namespace test {

class AudioMixerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mixer = std::make_unique<AudioMixer>();
    }

    void TearDown() override {
        if (mixer) {
            mixer->shutdown();
        }
    }

    std::unique_ptr<AudioMixer> mixer;
};

// --- Construction Tests ---

TEST_F(AudioMixerTest, DefaultConstruction) {
    EXPECT_FALSE(mixer->isInitialized());
    EXPECT_FALSE(mixer->isPlaying());
    EXPECT_FLOAT_EQ(mixer->getMasterVolume(), 1.0f);
    EXPECT_FALSE(mixer->isMasterMuted());
}

// --- Master Volume Tests (QM-029) ---

TEST_F(AudioMixerTest, SetMasterVolume) {
    mixer->setMasterVolume(0.5f);
    EXPECT_FLOAT_EQ(mixer->getMasterVolume(), 0.5f);
}

TEST_F(AudioMixerTest, MasterVolumeClampedToMin) {
    mixer->setMasterVolume(-0.5f);
    EXPECT_FLOAT_EQ(mixer->getMasterVolume(), 0.0f);
}

TEST_F(AudioMixerTest, MasterVolumeClampedToMax) {
    mixer->setMasterVolume(2.0f);
    EXPECT_FLOAT_EQ(mixer->getMasterVolume(), 1.0f);
}

TEST_F(AudioMixerTest, MasterVolumeDb) {
    mixer->setMasterVolume(1.0f);
    EXPECT_FLOAT_EQ(mixer->getMasterVolumeDb(), 0.0f);
}

TEST_F(AudioMixerTest, MasterVolumeDbHalf) {
    mixer->setMasterVolume(0.5f);
    EXPECT_NEAR(mixer->getMasterVolumeDb(), -6.02f, 0.1f);
}

TEST_F(AudioMixerTest, SetMasterVolumeDb) {
    mixer->setMasterVolumeDb(-6.0f);
    EXPECT_NEAR(mixer->getMasterVolume(), 0.5f, 0.02f);
}

TEST_F(AudioMixerTest, SetMasterVolumeDbZero) {
    mixer->setMasterVolumeDb(0.0f);
    EXPECT_FLOAT_EQ(mixer->getMasterVolume(), 1.0f);
}

// --- Master Mute Tests ---

TEST_F(AudioMixerTest, MasterMuteDefault) {
    EXPECT_FALSE(mixer->isMasterMuted());
}

TEST_F(AudioMixerTest, SetMasterMuted) {
    mixer->setMasterMuted(true);
    EXPECT_TRUE(mixer->isMasterMuted());

    mixer->setMasterMuted(false);
    EXPECT_FALSE(mixer->isMasterMuted());
}

TEST_F(AudioMixerTest, ToggleMasterMute) {
    EXPECT_FALSE(mixer->isMasterMuted());

    bool result = mixer->toggleMasterMute();
    EXPECT_TRUE(result);
    EXPECT_TRUE(mixer->isMasterMuted());

    result = mixer->toggleMasterMute();
    EXPECT_FALSE(result);
    EXPECT_FALSE(mixer->isMasterMuted());
}

// --- Source Management Tests (QM-029) ---

TEST_F(AudioMixerTest, AddSource) {
    auto source = std::make_shared<AudioSource>();
    mixer->addSource(source);

    EXPECT_EQ(mixer->getSourceCount(), 1u);
}

TEST_F(AudioMixerTest, AddMultipleSources) {
    auto source1 = std::make_shared<AudioSource>();
    auto source2 = std::make_shared<AudioSource>();
    auto source3 = std::make_shared<AudioSource>();

    mixer->addSource(source1);
    mixer->addSource(source2);
    mixer->addSource(source3);

    EXPECT_EQ(mixer->getSourceCount(), 3u);
}

TEST_F(AudioMixerTest, AddNullSource) {
    mixer->addSource(nullptr);
    EXPECT_EQ(mixer->getSourceCount(), 0u);
}

TEST_F(AudioMixerTest, RemoveSource) {
    auto source = std::make_shared<AudioSource>();
    UUID sourceId = source->getId();

    mixer->addSource(source);
    EXPECT_EQ(mixer->getSourceCount(), 1u);

    bool result = mixer->removeSource(sourceId);
    EXPECT_TRUE(result);
    EXPECT_EQ(mixer->getSourceCount(), 0u);
}

TEST_F(AudioMixerTest, RemoveNonexistentSource) {
    UUID fakeId = UUID::generate();
    bool result = mixer->removeSource(fakeId);
    EXPECT_FALSE(result);
}

TEST_F(AudioMixerTest, GetSource) {
    auto source = std::make_shared<AudioSource>();
    source->setName("TestSource");
    UUID sourceId = source->getId();

    mixer->addSource(source);

    auto retrieved = mixer->getSource(sourceId);
    EXPECT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->getName(), "TestSource");
}

TEST_F(AudioMixerTest, GetNonexistentSource) {
    UUID fakeId = UUID::generate();
    auto retrieved = mixer->getSource(fakeId);
    EXPECT_EQ(retrieved, nullptr);
}

TEST_F(AudioMixerTest, GetSources) {
    auto source1 = std::make_shared<AudioSource>();
    auto source2 = std::make_shared<AudioSource>();

    mixer->addSource(source1);
    mixer->addSource(source2);

    auto sources = mixer->getSources();
    EXPECT_EQ(sources.size(), 2u);
}

TEST_F(AudioMixerTest, ClearSources) {
    auto source1 = std::make_shared<AudioSource>();
    auto source2 = std::make_shared<AudioSource>();

    mixer->addSource(source1);
    mixer->addSource(source2);
    EXPECT_EQ(mixer->getSourceCount(), 2u);

    mixer->clearSources();
    EXPECT_EQ(mixer->getSourceCount(), 0u);
}

// --- Initialization Tests ---
// Note: These tests may fail in CI environments without audio hardware

TEST_F(AudioMixerTest, InitializeWithDefaults) {
    // This test may fail on systems without audio hardware
    bool result = mixer->initialize();
    // We don't assert success because audio hardware may not be available
    if (result) {
        EXPECT_TRUE(mixer->isInitialized());
    }
}

TEST_F(AudioMixerTest, ShutdownSafe) {
    // Shutdown should be safe even if not initialized
    mixer->shutdown();
    EXPECT_FALSE(mixer->isInitialized());
}

TEST_F(AudioMixerTest, DoubleInitialize) {
    // Initialize twice should be safe
    if (mixer->initialize()) {
        bool result = mixer->initialize();
        EXPECT_TRUE(result);  // Second init should return true
    }
}

TEST_F(AudioMixerTest, DoubleShutdown) {
    // Shutdown twice should be safe
    if (mixer->initialize()) {
        mixer->shutdown();
        mixer->shutdown();  // Should not crash
        EXPECT_FALSE(mixer->isInitialized());
    }
}

// --- Start/Stop Tests ---

TEST_F(AudioMixerTest, StartWithoutInit) {
    bool result = mixer->start();
    EXPECT_FALSE(result);  // Can't start without initialization
}

TEST_F(AudioMixerTest, StopWithoutStart) {
    if (mixer->initialize()) {
        mixer->stop();  // Should be safe
        EXPECT_FALSE(mixer->isPlaying());
    }
}

TEST_F(AudioMixerTest, StartAndStop) {
    if (mixer->initialize()) {
        bool startResult = mixer->start();
        if (startResult) {
            EXPECT_TRUE(mixer->isPlaying());

            mixer->stop();
            EXPECT_FALSE(mixer->isPlaying());
        }
    }
}

// --- Move Semantics Tests ---

TEST_F(AudioMixerTest, MoveConstruction) {
    // Create a new mixer for this test to avoid using the fixture mixer
    auto testMixer = std::make_unique<AudioMixer>();
    testMixer->setMasterVolume(0.7f);
    testMixer->setMasterMuted(true);

    // Note: Sources are part of impl, which gets moved
    AudioMixer moved(std::move(*testMixer));

    EXPECT_FLOAT_EQ(moved.getMasterVolume(), 0.7f);
    EXPECT_TRUE(moved.isMasterMuted());

    // Don't use testMixer after move - it's in moved-from state
}

// --- Thread Safety Tests ---

TEST_F(AudioMixerTest, ConcurrentSourceAccess) {
    // Add sources from multiple threads
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i]() {
            auto source = std::make_shared<AudioSource>();
            source->setName("Source" + std::to_string(i));
            mixer->addSource(source);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(mixer->getSourceCount(), 10u);
}

TEST_F(AudioMixerTest, ConcurrentVolumeChanges) {
    // Change volume from multiple threads
    std::vector<std::thread> threads;

    for (int i = 0; i < 100; ++i) {
        threads.emplace_back([this, i]() {
            float volume = static_cast<float>(i) / 100.0f;
            mixer->setMasterVolume(volume);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Volume should be between 0 and 1
    float vol = mixer->getMasterVolume();
    EXPECT_GE(vol, 0.0f);
    EXPECT_LE(vol, 1.0f);
}

// --- Constants Tests ---

TEST_F(AudioMixerTest, VolumeConstants) {
    EXPECT_FLOAT_EQ(AudioMixer::MIN_VOLUME, 0.0f);
    EXPECT_FLOAT_EQ(AudioMixer::MAX_VOLUME, 1.0f);
    EXPECT_FLOAT_EQ(AudioMixer::MIN_VOLUME_DB, -60.0f);
    EXPECT_FLOAT_EQ(AudioMixer::MAX_VOLUME_DB, 0.0f);
}

} // namespace test
} // namespace quadmapper
