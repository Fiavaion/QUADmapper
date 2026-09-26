#include <gtest/gtest.h>
#include "audio/AudioMixer.hpp"
#include "audio/AudioSource.hpp"
#include "audio/AudioBuffer.hpp"
#include "audio/AudioDevice.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
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

// --- Mixing (renderOffline drives the same path as the device callback) ---

namespace {

// A source whose every sample is `level` (both channels).
std::shared_ptr<AudioSource> constantSource(float level, size_t frames) {
    auto buffer = std::make_shared<AudioBuffer>(frames * 2);
    std::vector<float> samples(frames * 2, level);
    buffer->push(samples.data(), samples.size());
    auto source = std::make_shared<AudioSource>();
    source->setBuffer(buffer);
    source->setPan(-1.0f);  // hard left: left gain = volume, right = 0
    return source;
}

}  // namespace

TEST_F(AudioMixerTest, RenderOfflineMixesSourcesAndAppliesTheMasterVolume) {
    auto a = constantSource(0.25f, 4096);
    auto b = constantSource(0.25f, 4096);
    mixer->addSource(a);
    mixer->addSource(b);
    mixer->setMasterVolume(0.5f);
    std::vector<float> out(512 * 2);
    mixer->renderOffline(out.data(), 512, 0);  // first block starts at the set gains
    EXPECT_NEAR(out[0], 0.25f, 1e-5);          // (0.25 + 0.25) x 0.5, left only
    EXPECT_NEAR(out[1], 0.0f, 1e-6);
    EXPECT_NEAR(out[511 * 2], 0.25f, 1e-5);
}

TEST_F(AudioMixerTest, MutedSourcesAndMasterStillConsumeSoTheyStayInStep) {
    auto source = constantSource(0.5f, 4096);
    mixer->addSource(source);
    mixer->setMasterMuted(true);
    source->setMuted(true);
    std::vector<float> out(512 * 2);
    mixer->renderOffline(out.data(), 512, 0);
    for (float v : out) ASSERT_EQ(v, 0.0f);
    EXPECT_EQ(source->getBuffer()->available(), (4096u - 512u) * 2u);
}

TEST_F(AudioMixerTest, VolumeChangesRampAcrossOneBufferInsteadOfStepping) {
    auto source = constantSource(1.0f, 8192);
    mixer->addSource(source);
    std::vector<float> out(512 * 2);
    mixer->renderOffline(out.data(), 512, 0);
    ASSERT_NEAR(out[1022], 1.0f, 1e-5);

    source->setVolume(0.0f);  // a mute-sized jump in one go
    mixer->renderOffline(out.data(), 512, 0);
    float largestStep = 0.0f;
    float previous = 1.0f;
    for (size_t i = 0; i < 512; ++i) {
        largestStep = std::max(largestStep, std::abs(out[i * 2] - previous));
        previous = out[i * 2];
    }
    EXPECT_NEAR(out[511 * 2], 0.0f, 1e-5);
    EXPECT_LT(largestStep, 1.0f / 512.0f + 1e-5f);  // a straight ramp, no step

    mixer->setMasterVolume(0.0f);  // master ramps the same way
    source->setVolume(1.0f);
    mixer->renderOffline(out.data(), 512, 0);
    EXPECT_NEAR(out[511 * 2], 0.0f, 1e-5);
}

// removeSource() returns only once the callback cannot reach the source, so its owner
// may free it at once; and the source's last reference is dropped on the control
// thread, never in the callback. Hammered against a real (null backend) device thread.
TEST_F(AudioMixerTest, ARemovedSourceIsNeverRenderedAgainAndDiesOnTheControlThread) {
    struct Tracked : AudioSource {
        std::atomic<bool> removed{false};
        std::atomic<int>* violations = nullptr;
        std::atomic<int>* renders = nullptr;
        std::thread::id* diedOn = nullptr;
        ~Tracked() override { *diedOn = std::this_thread::get_id(); }
        void pull(float* out, size_t frames, const AudioRenderContext&) override {
            if (removed.load()) ++*violations;
            ++*renders;
            std::fill(out, out + frames * 2, 0.0f);
        }
    };

    AudioDevice device;
    ASSERT_TRUE(device.initialize(/*nullBackend=*/true));
    ASSERT_TRUE(mixer->initialize(&device));
    ASSERT_TRUE(mixer->start());

    std::atomic<int> violations{0};
    std::atomic<int> renders{0};
    int wrongThread = 0;
    // A fixed number of cycles, not a time budget: each waits for a callback, and under
    // ctest -j a 2 s budget ran as few as 88 (TD-23). The deadline only guards a hang.
    constexpr int CYCLES = 150;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int cycles = 0;
    while (cycles < CYCLES && std::chrono::steady_clock::now() < deadline) {
        std::thread::id diedOn;
        {
            auto source = std::make_shared<Tracked>();
            source->violations = &violations;
            source->renders = &renders;
            source->diedOn = &diedOn;
            mixer->addSource(source);
            std::this_thread::sleep_for(std::chrono::microseconds(500 + (cycles % 7) * 700));
            ASSERT_TRUE(mixer->removeSource(source->getId()));
            source->removed = true;
        }  // last reference dropped here
        if (diedOn != std::this_thread::get_id()) ++wrongThread;
        ++cycles;
    }
    mixer->shutdown();  // before `device`, whose context the device was opened on
    EXPECT_GT(renders.load(), 50);  // the device thread really was mixing
    EXPECT_EQ(cycles, CYCLES);
    EXPECT_EQ(violations.load(), 0);
    EXPECT_EQ(wrongThread, 0);
}

TEST_F(AudioMixerTest, NullBackendReportsItsDeviceAndLatency) {
    AudioDevice device;
    ASSERT_TRUE(device.initialize(true));
    ASSERT_TRUE(mixer->initialize(&device));
    EXPECT_FALSE(mixer->getDeviceName().empty());
    EXPECT_GT(mixer->getOutputLatency(), 0.0);
    EXPECT_LT(mixer->getOutputLatency(), 0.5);
    ASSERT_TRUE(mixer->start());
    EXPECT_FALSE(mixer->isDeviceLost());
    mixer->stop();  // a requested stop is not a lost device
    EXPECT_FALSE(mixer->isDeviceLost());
    mixer->shutdown();
}

TEST_F(AudioMixerTest, OpeningAnUnknownDeviceIdFails) {
    AudioDevice device;
    ASSERT_TRUE(device.initialize(true));
    EXPECT_FALSE(mixer->initialize(&device, "no-such-device"));
    EXPECT_FALSE(mixer->isInitialized());
}

} // namespace test
} // namespace quadmapper
