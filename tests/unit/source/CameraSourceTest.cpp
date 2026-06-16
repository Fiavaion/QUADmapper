// QM-055: live camera source + device enumeration.
//
// The enumeration test always runs (pure, no hardware needed). The live-open
// test exercises the real capture path but SKIPS (never fails) when no device
// is present or the camera is busy, so CI stays green and dev machines still
// get real verification. All loops are bounded — no hang risk.
#include <gtest/gtest.h>
#include "source/CameraDevices.hpp"
#include "source/CameraSource.hpp"
#include "source/FrameData.hpp"

#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <cstdint>

using namespace quadmapper;

TEST(CameraDevicesTest, EnumerateDoesNotCrash) {
    auto devices = enumerateCameraDevices();
    // Can't assert a count (hardware-dependent), but names must be non-empty.
    for (const auto& d : devices) {
        EXPECT_FALSE(d.name.empty());
    }
    SUCCEED() << "Found " << devices.size() << " capture device(s)";
}

TEST(CameraSourceTest, OpensDefaultDeviceAndProducesFrame) {
    auto devices = enumerateCameraDevices();
    if (devices.empty()) {
        GTEST_SKIP() << "No capture device present";
    }

    CameraSource cam("");  // empty -> first enumerated device
    if (!cam.initialize()) {
        GTEST_SKIP() << "Camera present but could not be opened (in use?)";
    }

    EXPECT_TRUE(cam.isReady());
    EXPECT_GT(cam.getWidth(), 0);
    EXPECT_GT(cam.getHeight(), 0);
    EXPECT_FALSE(cam.getDeviceName().empty());

    // Pull frames; live capture needs a moment to deliver the first one.
    bool gotFrame = false;
    for (int i = 0; i < 90 && !gotFrame; ++i) {
        if (cam.update(0.016) && cam.hasNewFrame()) {
            const FrameData* f = cam.getCurrentFrame();
            ASSERT_NE(f, nullptr);
            EXPECT_TRUE(f->isValid());
            EXPECT_EQ(f->channels, 3);
            EXPECT_EQ(f->pixels.size(), f->getDataSize());
            EXPECT_EQ(f->width, cam.getWidth());
            EXPECT_EQ(f->height, cam.getHeight());
            gotFrame = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    EXPECT_TRUE(gotFrame) << "No frame delivered within ~1.8s";

    // AC2: capture the current frame to a PNG (the AI sidecar's input).
    if (gotFrame) {
        auto pngPath = std::filesystem::temp_directory_path() / "qm055_camera_capture.png";
        std::error_code ec;
        std::filesystem::remove(pngPath, ec);

        ASSERT_TRUE(cam.captureCurrentFrameToPng(pngPath));
        ASSERT_TRUE(std::filesystem::exists(pngPath));
        EXPECT_GT(std::filesystem::file_size(pngPath), 0u);

        // Verify real PNG signature: 89 50 4E 47 0D 0A 1A 0A
        std::ifstream in(pngPath, std::ios::binary);
        unsigned char sig[8] = {0};
        in.read(reinterpret_cast<char*>(sig), 8);
        const unsigned char png[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(sig[i], png[i]) << "PNG signature mismatch at byte " << i;
        }
        in.close();
        std::filesystem::remove(pngPath, ec);
    }

    cam.shutdown();
    EXPECT_FALSE(cam.isReady());
}
