#include <gtest/gtest.h>
#include "audio/AudioBuffer.hpp"
#include <thread>
#include <vector>
#include <atomic>

namespace quadmapper {
namespace test {

class AudioBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer = std::make_unique<AudioBuffer>(1024);
    }

    std::unique_ptr<AudioBuffer> buffer;
};

TEST_F(AudioBufferTest, DefaultState) {
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_FALSE(buffer->isFull());
    EXPECT_EQ(buffer->available(), 0u);
    EXPECT_EQ(buffer->capacity(), 1024u);
    EXPECT_FALSE(buffer->isEndOfStream());
}

TEST_F(AudioBufferTest, PushAndRead) {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};

    EXPECT_TRUE(buffer->push(samples.data(), samples.size(), 0.0));
    EXPECT_FALSE(buffer->isEmpty());
    EXPECT_EQ(buffer->available(), 4u);

    std::vector<float> output(4);
    size_t read = buffer->read(output.data(), 4, 2);

    EXPECT_EQ(read, 4u);
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_FLOAT_EQ(output[0], 0.1f);
    EXPECT_FLOAT_EQ(output[1], 0.2f);
    EXPECT_FLOAT_EQ(output[2], 0.3f);
    EXPECT_FLOAT_EQ(output[3], 0.4f);
}

TEST_F(AudioBufferTest, PartialRead) {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    buffer->push(samples.data(), samples.size(), 0.0);

    std::vector<float> output(4);
    size_t read = buffer->read(output.data(), 4, 2);

    EXPECT_EQ(read, 4u);
    EXPECT_EQ(buffer->available(), 2u);
}

TEST_F(AudioBufferTest, ReadMoreThanAvailable) {
    std::vector<float> samples = {0.1f, 0.2f};
    buffer->push(samples.data(), samples.size(), 0.0);

    std::vector<float> output(10);
    size_t read = buffer->read(output.data(), 10, 2);

    EXPECT_EQ(read, 2u);
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, ReadFromEmpty) {
    std::vector<float> output(4);
    size_t read = buffer->read(output.data(), 4, 2);

    EXPECT_EQ(read, 0u);
}

TEST_F(AudioBufferTest, PushToFull) {
    std::vector<float> samples(1024, 0.5f);
    EXPECT_TRUE(buffer->push(samples.data(), samples.size(), 0.0));
    EXPECT_TRUE(buffer->isFull());

    // Push to full buffer should fail
    std::vector<float> moreSamples = {0.1f, 0.2f};
    EXPECT_FALSE(buffer->push(moreSamples.data(), moreSamples.size(), 0.0));
}

TEST_F(AudioBufferTest, WrapAround) {
    // Fill half the buffer
    std::vector<float> first(600, 0.1f);
    buffer->push(first.data(), first.size(), 0.0);

    // Read most of it
    std::vector<float> output(500);
    buffer->read(output.data(), 500, 2);

    // Push more data that wraps around
    std::vector<float> second(800, 0.2f);
    EXPECT_TRUE(buffer->push(second.data(), second.size(), 0.0));
    EXPECT_EQ(buffer->available(), 100u + 800u);

    // Read all
    std::vector<float> allOutput(900);
    size_t read = buffer->read(allOutput.data(), 900, 2);
    EXPECT_EQ(read, 900u);
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, Clear) {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    buffer->push(samples.data(), samples.size(), 0.0);

    buffer->clear();

    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_EQ(buffer->available(), 0u);
}

TEST_F(AudioBufferTest, EndOfStream) {
    EXPECT_FALSE(buffer->isEndOfStream());

    buffer->signalEndOfStream();

    EXPECT_TRUE(buffer->isEndOfStream());

    buffer->resetEndOfStream();

    EXPECT_FALSE(buffer->isEndOfStream());
}

TEST_F(AudioBufferTest, GetCurrentPTS) {
    EXPECT_DOUBLE_EQ(buffer->getCurrentPTS(), -1.0);  // Empty buffer

    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    buffer->push(samples.data(), samples.size(), 2.5);

    EXPECT_DOUBLE_EQ(buffer->getCurrentPTS(), 2.5);
}

TEST_F(AudioBufferTest, PushNullPointer) {
    EXPECT_TRUE(buffer->push(nullptr, 10, 0.0));  // Should handle gracefully
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, PushZeroCount) {
    std::vector<float> samples = {0.1f};
    EXPECT_TRUE(buffer->push(samples.data(), 0, 0.0));
    EXPECT_TRUE(buffer->isEmpty());
}

TEST_F(AudioBufferTest, ReadNullPointer) {
    std::vector<float> samples = {0.1f, 0.2f};
    buffer->push(samples.data(), samples.size(), 0.0);

    EXPECT_EQ(buffer->read(nullptr, 10, 2), 0u);
    EXPECT_EQ(buffer->available(), 2u);  // Data still there
}

TEST_F(AudioBufferTest, ReadZeroCount) {
    std::vector<float> samples = {0.1f, 0.2f};
    buffer->push(samples.data(), samples.size(), 0.0);

    std::vector<float> output(10);
    EXPECT_EQ(buffer->read(output.data(), 0, 2), 0u);
    EXPECT_EQ(buffer->available(), 2u);
}

// Thread safety tests
TEST_F(AudioBufferTest, ConcurrentPushAndRead) {
    auto largeBuffer = std::make_unique<AudioBuffer>(100000);
    std::atomic<size_t> totalPushed{0};
    std::atomic<size_t> totalRead{0};
    std::atomic<bool> done{false};

    // Producer thread
    std::thread producer([&]() {
        std::vector<float> samples(1000, 0.5f);
        for (int i = 0; i < 50; ++i) {
            while (!largeBuffer->push(samples.data(), samples.size(), 0.0)) {
                std::this_thread::yield();
            }
            totalPushed += samples.size();
        }
        done = true;
    });

    // Consumer thread
    std::thread consumer([&]() {
        std::vector<float> output(500);
        while (!done || !largeBuffer->isEmpty()) {
            size_t read = largeBuffer->read(output.data(), output.size(), 2);
            totalRead += read;
            if (read == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(totalPushed.load(), totalRead.load());
}

TEST_F(AudioBufferTest, BlockingPush) {
    auto smallBuffer = std::make_unique<AudioBuffer>(100);
    std::vector<float> samples(60, 0.5f);

    // Fill buffer partially
    smallBuffer->push(samples.data(), samples.size(), 0.0);

    // Start a thread that will read after a delay
    std::thread reader([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::vector<float> output(50);
        smallBuffer->read(output.data(), output.size(), 2);
    });

    // This should block until reader makes space
    std::vector<float> moreSamples(50, 0.6f);
    bool result = smallBuffer->pushBlocking(moreSamples.data(), moreSamples.size(), 0.0, 1000);

    reader.join();
    EXPECT_TRUE(result);
}

TEST_F(AudioBufferTest, BlockingPushTimeout) {
    auto smallBuffer = std::make_unique<AudioBuffer>(100);
    std::vector<float> samples(100, 0.5f);
    smallBuffer->push(samples.data(), samples.size(), 0.0);

    // Try to push to full buffer with short timeout
    std::vector<float> moreSamples(10, 0.6f);
    bool result = smallBuffer->pushBlocking(moreSamples.data(), moreSamples.size(), 0.0, 50);

    EXPECT_FALSE(result);  // Should timeout
}

TEST_F(AudioBufferTest, BlockingReadTimeout) {
    std::vector<float> output(10);
    size_t read = buffer->readBlocking(output.data(), 10, 2, 50);

    EXPECT_EQ(read, 0u);  // Should timeout on empty buffer
}

TEST_F(AudioBufferTest, BlockingReadEndOfStream) {
    std::thread signaler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        buffer->signalEndOfStream();
    });

    std::vector<float> output(10);
    size_t read = buffer->readBlocking(output.data(), 10, 2, 1000);

    signaler.join();
    EXPECT_EQ(read, 0u);  // Should return when EOS signaled
}

} // namespace test
} // namespace quadmapper
