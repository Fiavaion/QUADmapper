#include <gtest/gtest.h>
#include "audio/AudioSource.hpp"
#include "audio/AudioBuffer.hpp"
#include <cmath>

namespace quadmapper {
namespace test {

class AudioSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        source = std::make_unique<AudioSource>();
    }

    std::unique_ptr<AudioSource> source;
};

// --- Construction Tests ---

TEST_F(AudioSourceTest, DefaultConstruction) {
    EXPECT_FALSE(source->getId().isNull());
    EXPECT_EQ(source->getName(), "");
    EXPECT_FLOAT_EQ(source->getVolume(), 1.0f);
    EXPECT_FALSE(source->isMuted());
    EXPECT_TRUE(source->isEnabled());
    EXPECT_FLOAT_EQ(source->getPan(), 0.0f);
    EXPECT_EQ(source->getBuffer(), nullptr);
}

TEST_F(AudioSourceTest, ConstructionWithId) {
    UUID customId = UUID::generate();
    AudioSource customSource(customId);
    EXPECT_EQ(customSource.getId(), customId);
}

TEST_F(AudioSourceTest, UniqueIds) {
    AudioSource source1;
    AudioSource source2;
    EXPECT_NE(source1.getId(), source2.getId());
}

// --- Name Tests ---

TEST_F(AudioSourceTest, SetName) {
    source->setName("Test Source");
    EXPECT_EQ(source->getName(), "Test Source");
}

// --- Volume Tests (QM-030) ---

TEST_F(AudioSourceTest, SetVolume) {
    source->setVolume(0.5f);
    EXPECT_FLOAT_EQ(source->getVolume(), 0.5f);
}

TEST_F(AudioSourceTest, VolumeClampedToMin) {
    source->setVolume(-0.5f);
    EXPECT_FLOAT_EQ(source->getVolume(), 0.0f);
}

TEST_F(AudioSourceTest, VolumeClampedToMax) {
    source->setVolume(2.0f);
    EXPECT_FLOAT_EQ(source->getVolume(), 1.0f);
}

TEST_F(AudioSourceTest, VolumeZero) {
    source->setVolume(0.0f);
    EXPECT_FLOAT_EQ(source->getVolume(), 0.0f);
}

TEST_F(AudioSourceTest, VolumeDb) {
    // Full volume = 0 dB
    source->setVolume(1.0f);
    EXPECT_FLOAT_EQ(source->getVolumeDb(), 0.0f);
}

TEST_F(AudioSourceTest, VolumeDbHalf) {
    // Half volume = -6.02 dB (approximately)
    source->setVolume(0.5f);
    EXPECT_NEAR(source->getVolumeDb(), -6.02f, 0.1f);
}

TEST_F(AudioSourceTest, VolumeDbMin) {
    source->setVolume(0.0f);
    EXPECT_FLOAT_EQ(source->getVolumeDb(), AudioSource::MIN_VOLUME_DB);
}

TEST_F(AudioSourceTest, SetVolumeDb) {
    source->setVolumeDb(-6.0f);
    EXPECT_NEAR(source->getVolume(), 0.5f, 0.02f);
}

TEST_F(AudioSourceTest, SetVolumeDbZero) {
    source->setVolumeDb(0.0f);
    EXPECT_FLOAT_EQ(source->getVolume(), 1.0f);
}

TEST_F(AudioSourceTest, SetVolumeDbMin) {
    source->setVolumeDb(-100.0f);  // Should clamp to MIN_VOLUME_DB
    EXPECT_GT(source->getVolume(), 0.0f);  // Not quite zero
}

// --- Mute Tests (QM-030) ---

TEST_F(AudioSourceTest, MuteDefault) {
    EXPECT_FALSE(source->isMuted());
}

TEST_F(AudioSourceTest, SetMuted) {
    source->setMuted(true);
    EXPECT_TRUE(source->isMuted());

    source->setMuted(false);
    EXPECT_FALSE(source->isMuted());
}

TEST_F(AudioSourceTest, ToggleMute) {
    EXPECT_FALSE(source->isMuted());

    bool result = source->toggleMute();
    EXPECT_TRUE(result);
    EXPECT_TRUE(source->isMuted());

    result = source->toggleMute();
    EXPECT_FALSE(result);
    EXPECT_FALSE(source->isMuted());
}

// --- Enabled Tests ---

TEST_F(AudioSourceTest, EnabledDefault) {
    EXPECT_TRUE(source->isEnabled());
}

TEST_F(AudioSourceTest, SetEnabled) {
    source->setEnabled(false);
    EXPECT_FALSE(source->isEnabled());

    source->setEnabled(true);
    EXPECT_TRUE(source->isEnabled());
}

// --- Pan Tests ---

TEST_F(AudioSourceTest, PanDefault) {
    EXPECT_FLOAT_EQ(source->getPan(), 0.0f);
}

TEST_F(AudioSourceTest, SetPan) {
    source->setPan(-0.5f);
    EXPECT_FLOAT_EQ(source->getPan(), -0.5f);

    source->setPan(0.5f);
    EXPECT_FLOAT_EQ(source->getPan(), 0.5f);
}

TEST_F(AudioSourceTest, PanClampedToMin) {
    source->setPan(-2.0f);
    EXPECT_FLOAT_EQ(source->getPan(), -1.0f);
}

TEST_F(AudioSourceTest, PanClampedToMax) {
    source->setPan(2.0f);
    EXPECT_FLOAT_EQ(source->getPan(), 1.0f);
}

// --- Effective Volume Tests ---

TEST_F(AudioSourceTest, EffectiveVolumeNormal) {
    source->setVolume(0.8f);
    EXPECT_FLOAT_EQ(source->getEffectiveVolume(), 0.8f);
}

TEST_F(AudioSourceTest, EffectiveVolumeWhenMuted) {
    source->setVolume(0.8f);
    source->setMuted(true);
    EXPECT_FLOAT_EQ(source->getEffectiveVolume(), 0.0f);
}

TEST_F(AudioSourceTest, EffectiveVolumeWhenDisabled) {
    source->setVolume(0.8f);
    source->setEnabled(false);
    EXPECT_FLOAT_EQ(source->getEffectiveVolume(), 0.0f);
}

// --- Gain Tests (Pan + Volume) ---

TEST_F(AudioSourceTest, GainsCenter) {
    source->setVolume(1.0f);
    source->setPan(0.0f);

    // At center pan, both channels should have equal gain
    float left = source->getLeftGain();
    float right = source->getRightGain();

    EXPECT_NEAR(left, right, 0.01f);
    EXPECT_GT(left, 0.0f);
}

TEST_F(AudioSourceTest, GainsFullLeft) {
    source->setVolume(1.0f);
    source->setPan(-1.0f);

    float left = source->getLeftGain();
    float right = source->getRightGain();

    EXPECT_GT(left, right);
    EXPECT_NEAR(right, 0.0f, 0.01f);
}

TEST_F(AudioSourceTest, GainsFullRight) {
    source->setVolume(1.0f);
    source->setPan(1.0f);

    float left = source->getLeftGain();
    float right = source->getRightGain();

    EXPECT_LT(left, right);
    EXPECT_NEAR(left, 0.0f, 0.01f);
}

TEST_F(AudioSourceTest, GainsWithMute) {
    source->setVolume(1.0f);
    source->setMuted(true);

    EXPECT_FLOAT_EQ(source->getLeftGain(), 0.0f);
    EXPECT_FLOAT_EQ(source->getRightGain(), 0.0f);
}

TEST_F(AudioSourceTest, GainsWithVolumeScaling) {
    source->setVolume(0.5f);
    source->setPan(0.0f);

    float left1 = source->getLeftGain();

    source->setVolume(1.0f);
    float left2 = source->getLeftGain();

    EXPECT_NEAR(left1, left2 * 0.5f, 0.01f);
}

// --- Buffer Tests ---

TEST_F(AudioSourceTest, SetBuffer) {
    auto buffer = std::make_shared<AudioBuffer>(1024);
    source->setBuffer(buffer);
    EXPECT_EQ(source->getBuffer(), buffer);
}

TEST_F(AudioSourceTest, BufferIsShared) {
    auto buffer = std::make_shared<AudioBuffer>(1024);
    source->setBuffer(buffer);

    // Fill buffer through the original shared_ptr
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    buffer->push(samples.data(), samples.size());

    // Should see data through source's buffer reference
    EXPECT_EQ(source->getBuffer()->available(), 4u);
}

} // namespace test
} // namespace quadmapper
