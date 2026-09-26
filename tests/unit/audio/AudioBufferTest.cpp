#include <gtest/gtest.h>
#include "audio/AudioBuffer.hpp"
#include <atomic>
#include <thread>
#include <vector>

namespace quadmapper {
namespace test {

class AudioBufferTest : public ::testing::Test {
protected:
    void SetUp() override { buffer = std::make_unique<AudioBuffer>(1024); }

    std::unique_ptr<AudioBuffer> buffer;
};

TEST_F(AudioBufferTest, DefaultState) {
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_FALSE(buffer->isFull());
    EXPECT_EQ(buffer->available(), 0u);
    EXPECT_EQ(buffer->capacity(), 1024u);
    EXPECT_EQ(buffer->freeSpace(), 1024u);
    EXPECT_EQ(buffer->writePosition(), 0u);
    EXPECT_EQ(buffer->readPosition(), 0u);
}

TEST_F(AudioBufferTest, PushAndRead) {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};

    EXPECT_TRUE(buffer->push(samples.data(), samples.size()));
    EXPECT_FALSE(buffer->isEmpty());
    EXPECT_EQ(buffer->available(), 4u);

    std::vector<float> output(4);
    EXPECT_EQ(buffer->read(output.data(), 4), 4u);
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_EQ(output, samples);
}

TEST_F(AudioBufferTest, PartialRead) {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    buffer->push(samples.data(), samples.size());

    std::vector<float> output(4);
    EXPECT_EQ(buffer->read(output.data(), 4), 4u);
    EXPECT_EQ(buffer->available(), 2u);
}

TEST_F(AudioBufferTest, ReadMoreThanAvailable) {
    std::vector<float> samples = {0.1f, 0.2f};
    buffer->push(samples.data(), samples.size());

    std::vector<float> output(10);
    EXPECT_EQ(buffer->read(output.data(), 10), 2u);
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, ReadFromEmpty) {
    std::vector<float> output(4);
    EXPECT_EQ(buffer->read(output.data(), 4), 0u);
}

TEST_F(AudioBufferTest, PushToFullIsAllOrNothing) {
    std::vector<float> samples(1024, 0.5f);
    EXPECT_TRUE(buffer->push(samples.data(), samples.size()));
    EXPECT_TRUE(buffer->isFull());

    std::vector<float> moreSamples = {0.1f, 0.2f};
    EXPECT_FALSE(buffer->push(moreSamples.data(), moreSamples.size()));
    EXPECT_EQ(buffer->available(), 1024u);
}

TEST_F(AudioBufferTest, WrapAroundKeepsOrder) {
    std::vector<float> first(600);
    for (size_t i = 0; i < first.size(); ++i) first[i] = static_cast<float>(i);
    buffer->push(first.data(), first.size());

    std::vector<float> output(500);
    buffer->read(output.data(), 500);

    std::vector<float> second(800);
    for (size_t i = 0; i < second.size(); ++i) second[i] = 1000.0f + static_cast<float>(i);
    EXPECT_TRUE(buffer->push(second.data(), second.size()));
    EXPECT_EQ(buffer->available(), 100u + 800u);

    std::vector<float> all(900);
    EXPECT_EQ(buffer->read(all.data(), 900), 900u);
    EXPECT_FLOAT_EQ(all[0], 500.0f);
    EXPECT_FLOAT_EQ(all[99], 599.0f);
    EXPECT_FLOAT_EQ(all[100], 1000.0f);
    EXPECT_FLOAT_EQ(all[899], 1799.0f);
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, PositionsCountEverySample) {
    std::vector<float> samples(300, 0.0f);
    std::vector<float> output(300);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(buffer->push(samples.data(), samples.size()));
        ASSERT_EQ(buffer->read(output.data(), output.size()), 300u);
    }
    // Positions never wrap: they name a sample of the stream for good.
    EXPECT_EQ(buffer->writePosition(), 3000u);
    EXPECT_EQ(buffer->readPosition(), 3000u);
}

TEST_F(AudioBufferTest, DiscardToSkipsForwardOnly) {
    std::vector<float> samples(100);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(i);
    buffer->push(samples.data(), samples.size());

    buffer->discardTo(40);
    EXPECT_EQ(buffer->readPosition(), 40u);
    EXPECT_EQ(buffer->available(), 60u);

    buffer->discardTo(10);  // never moves back
    EXPECT_EQ(buffer->readPosition(), 40u);

    float next = -1.0f;
    ASSERT_EQ(buffer->read(&next, 1), 1u);
    EXPECT_FLOAT_EQ(next, 40.0f);

    buffer->discardTo(5000);  // clamped to what has been pushed
    EXPECT_EQ(buffer->readPosition(), 100u);
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, NullAndEmptyArgumentsAreHarmless) {
    EXPECT_TRUE(buffer->push(nullptr, 10));
    std::vector<float> samples = {0.1f, 0.2f};
    EXPECT_TRUE(buffer->push(samples.data(), 0));
    EXPECT_TRUE(buffer->isEmpty());

    buffer->push(samples.data(), samples.size());
    std::vector<float> output(10);
    EXPECT_EQ(buffer->read(nullptr, 10), 0u);
    EXPECT_EQ(buffer->read(output.data(), 0), 0u);
    EXPECT_EQ(buffer->available(), 2u);
}

// One producer and one consumer, as the decode thread and the audio callback run it:
// every sample arrives once, in order.
TEST_F(AudioBufferTest, ConcurrentPushAndReadKeepsEverySampleInOrder) {
    AudioBuffer ring(4096);
    constexpr size_t TOTAL = 200000;
    std::atomic<bool> done{false};
    size_t mismatches = 0;
    size_t received = 0;

    std::thread producer([&]() {
        std::vector<float> chunk(700);
        size_t next = 0;
        while (next < TOTAL) {
            const size_t n = std::min(chunk.size(), TOTAL - next);
            for (size_t i = 0; i < n; ++i) chunk[i] = static_cast<float>((next + i) % 100000);
            while (!ring.push(chunk.data(), n)) std::this_thread::yield();
            next += n;
        }
        done = true;
    });

    std::thread consumer([&]() {
        std::vector<float> output(512);
        while (!done || !ring.isEmpty()) {
            const size_t n = ring.read(output.data(), output.size());
            for (size_t i = 0; i < n; ++i) {
                if (output[i] != static_cast<float>((received + i) % 100000)) ++mismatches;
            }
            received += n;
            if (n == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(received, TOTAL);
    EXPECT_EQ(mismatches, 0u);
}

}  // namespace test
}  // namespace quadmapper
