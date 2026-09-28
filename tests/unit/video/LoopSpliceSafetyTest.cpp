// Loop-splice safety regression tests (TD-30 fresh-context review). The Loop splice
// (VideoDecoder::spliceNextPass) feeds a clip's first packet straight after its last one
// instead of flushing and seeking; that is only safe when the start is a clean entry
// point (IDR/BLA for H.264/HEVC — startsCleanly(), gated by readFirstFrame()'s first-
// packet check). The existing VideoDecoderLoopSeam tests (VideoPlayerPlaybackTest.cpp)
// all encode mpeg2video at test time, which never reaches the H.264/HEVC NAL-parsing
// branch: startsCleanly() and readFirstFrame()'s check can be replaced with `return
// true` / `first = true` there without failing anything. These tests exercise real
// H.264/HEVC streams instead.
//
// The linked FFmpeg build has no libx264/libx265 encoder (see vcpkg.json — no
// "x264"/"x265" feature), so the clips are pre-made fixtures in tests/fixtures/videos
// (a system FFmpeg build made them; a few KB to 40 KB each):
//   - h264_open_gop.mp4    : cut from an x264 stream encoded with open-gop=1, landing on
//                            a non-IDR I-frame (a true open-GOP re-entry point).
//   - hevc_cra_open_gop.ts : cut from an x265 stream (open GOP is its default), landing
//                            on a CRA picture with RASL pictures referring before it.
//   - h264_closed_gop.mp4  : x264, no-open-gop=1 — every keyframe is IDR.
//   - hevc_closed_gop.mp4  : x265, no-open-gop=1 — every keyframe is IDR.
//
// Every test compares the frames played across several Loop seams against a plain
// (Once) decode of the same file: presence and pixels (by a checksum keyed on pts),
// decode order, and the analytic `due` timeline VideoDecoder stamps each frame with.

#include <gtest/gtest.h>

#include "video/VideoDecoder.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) { return fs::path(QM_VIDEO_FIXTURES_DIR) / name; }

uint64_t hashOf(const std::vector<uint8_t>& d) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : d) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

bool nextBuffered(VideoDecoder& dec, VideoFrame& out,
                   std::chrono::milliseconds limit = std::chrono::milliseconds(4000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (dec.getBufferedFrameCount() > 0) {
            out = dec.getBufferedFrame();
            if (out.isValid()) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

struct Reference {
    std::vector<double> pts;
    std::vector<uint64_t> hash;
    std::map<long, int> byMs;  // pts rounded to a millisecond -> frame index
    double duration = 0.0;
    double fps = 25.0;
};

// A plain, single-pass (Once, synchronous) decode of the whole clip: the ground truth
// every frame played in Loop mode across a seam is checked against.
Reference decodeReference(const fs::path& path) {
    Reference ref;
    VideoDecoder decoder;
    if (!decoder.open(path)) return ref;
    for (VideoFrame f = decoder.decodeNextFrame(); f.isValid(); f = decoder.decodeNextFrame()) {
        ref.byMs[std::lround(f.pts * 1000)] = static_cast<int>(ref.pts.size());
        ref.pts.push_back(f.pts);
        ref.hash.push_back(hashOf(f.data));
    }
    ref.duration = decoder.getInfo().duration;
    if (ref.pts.size() > 1) ref.fps = 1.0 / (ref.pts[1] - ref.pts[0]);
    decoder.close();
    return ref;
}

struct LoopProbe {
    bool opened = false;
    int framesChecked = 0;
    int badPix = 0;    // frame's pixels do not match the reference decode at that pts
    int badOrder = 0;  // frame index does not follow the previous one, wrapping at the end
    int badDue = 0;    // VideoFrame::due is not (pass * duration + pts), or did not advance
    int missing = 0;   // pts not found in the reference decode at all
    uint64_t restartsDelta = 0;
};

// Plays `path` in Loop mode from `startBackFrames` before the end, across several seams,
// and checks every frame against `ref` (see LoopProbe).
LoopProbe probeLoop(const fs::path& path, const Reference& ref, int startBackFrames,
                     int seamsWanted, const DecodeOptions& options = {}) {
    LoopProbe result;
    VideoDecoder decoder;
    if (!decoder.open(path, options)) return result;
    result.opened = true;

    const int n = static_cast<int>(ref.pts.size());
    DecodePlan plan;
    plan.duration = ref.duration;
    plan.loopMode = LoopMode::LOOP;
    plan.timeline = std::max(0.0, ref.duration - startBackFrames / ref.fps);
    decoder.startPlayback(plan, 8);

    VideoFrame frame;
    if (!nextBuffered(decoder, frame)) {
        decoder.stopBackgroundDecoding();
        return result;
    }
    const uint64_t restarts0 = decoder.decoderRestarts();

    int prevIndex = -1;
    double prevDue = -1e18;
    long prevMs = std::numeric_limits<long>::min();
    const int want = n * seamsWanted + startBackFrames + 4;
    for (int i = 0; i < want; ++i) {
        if (i > 0 && !nextBuffered(decoder, frame)) break;
        ++result.framesChecked;

        // A frame whose pts VideoDecoder could not read (options.testStripPtsAfterSplice)
        // reports the same fallback pts as the frame before it (VideoDecoder::convertFrame:
        // result.pts = currentTime_ before it is advanced) — a known, harmless quirk of
        // that test hook, not a real repeated frame. Nothing else is dropped or reordered
        // by it, so its identity is still known (the next one after the frame before it);
        // only the pts-keyed lookup is skipped, since it cannot tell the two apart.
        const long ms = std::lround(frame.pts * 1000);
        const bool strippedPtsArtifact = ms == prevMs && prevIndex >= 0;
        prevMs = ms;
        const int index = strippedPtsArtifact ? (prevIndex + 1) % n
                                               : [&]() -> int {
                                                     auto it = ref.byMs.find(ms);
                                                     return it == ref.byMs.end() ? -1 : it->second;
                                                 }();
        if (index < 0) {
            ++result.missing;
        } else {
            if (ref.hash[index] != hashOf(frame.data)) ++result.badPix;
            if (prevIndex >= 0 && index != (prevIndex + 1) % n) ++result.badOrder;
            prevIndex = index;
        }
        if (strippedPtsArtifact) continue;  // due is not on the pts grid for this one (expected)

        const long pass = std::lround((frame.due - frame.pts) / ref.duration);
        const bool onGrid = std::abs(frame.due - (pass * ref.duration + frame.pts)) < 1e-6;
        if (!onGrid || frame.due <= prevDue) ++result.badDue;
        prevDue = frame.due;
    }
    result.restartsDelta = decoder.decoderRestarts() - restarts0;
    decoder.stopBackgroundDecoding();
    return result;
}

}  // namespace

// --- (a) H.264: a first frame that is a non-IDR I-frame (open GOP) must refuse the
// splice and restart the codec instead, at every seam, decoding correctly regardless. ---

TEST(LoopSpliceSafety, H264OpenGopRefusesSpliceAndDecodesCorrectly) {
    const fs::path path = fixture("h264_open_gop.mp4");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    const LoopProbe probe = probeLoop(path, ref, 3, 3);
    ASSERT_TRUE(probe.opened);
    EXPECT_GT(probe.restartsDelta, 0u) << "the splice must refuse an open-GOP entry point";
    EXPECT_EQ(probe.badPix, 0);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
}

// --- (b) HEVC: a CRA entry point (RASL pictures after it refer before it) must refuse
// the splice the same way. ---

TEST(LoopSpliceSafety, HevcCraCutRefusesSpliceAndDecodesCorrectly) {
    const fs::path path = fixture("hevc_cra_open_gop.ts");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    const LoopProbe probe = probeLoop(path, ref, 3, 3);
    ASSERT_TRUE(probe.opened);
    EXPECT_GT(probe.restartsDelta, 0u) << "the splice must refuse a CRA entry point";
    EXPECT_EQ(probe.badPix, 0);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
}

// --- (c) closed-GOP H.264/HEVC: every keyframe is IDR, so the splice is safe and must
// never restart the codec at the seam (the point of TD-30). ---

TEST(LoopSpliceSafety, H264ClosedGopSplicesWithoutRestart) {
    const fs::path path = fixture("h264_closed_gop.mp4");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    const LoopProbe probe = probeLoop(path, ref, 3, 3);
    ASSERT_TRUE(probe.opened);
    EXPECT_EQ(probe.restartsDelta, 0u) << "a closed-GOP IDR entry should splice with no restart";
    EXPECT_EQ(probe.badPix, 0);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
}

TEST(LoopSpliceSafety, HevcClosedGopSplicesWithoutRestart) {
    const fs::path path = fixture("hevc_closed_gop.mp4");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    const LoopProbe probe = probeLoop(path, ref, 3, 3);
    ASSERT_TRUE(probe.opened);
    EXPECT_EQ(probe.restartsDelta, 0u) << "a closed-GOP IDR entry should splice with no restart";
    EXPECT_EQ(probe.badPix, 0);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
}

// --- Finding 2: a frame with no timestamp right at the seam must not strand the splice
// (the leg would then never advance and every later frame carries the wrong `due`). ---

TEST(LoopSpliceSafety, NoPtsFrameDuringPendingSpliceDoesNotStallTheLeg) {
    const fs::path path = fixture("h264_closed_gop.mp4");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    DecodeOptions options;
    options.testStripPtsAfterSplice = true;  // NOPTS the first frame decoded after each splice
    const LoopProbe probe = probeLoop(path, ref, 3, 4, options);
    ASSERT_TRUE(probe.opened);
    // Reaching every requested frame across four seams (not stopping early, and not
    // getting stuck emitting frames tagged with a stale leg) is the point: before the
    // fix, losing lastForwardPts_ on the stripped frame left splicePending_ set forever,
    // so the leg never advanced and every frame after it carried the wrong `due`.
    EXPECT_GE(probe.framesChecked, static_cast<int>(ref.pts.size()) * 4);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
}

// --- readFirstFrame()'s "first" check: a splice seek that lands on a later keyframe
// (as an MPEG-TS backward seek can overshoot to, per the comment in spliceNextPass())
// must be refused rather than spliced in as if it were the clip's start. ---

TEST(LoopSpliceSafety, SeekOvershootPastTheStartIsRefused) {
    const fs::path path = fixture("h264_closed_gop.mp4");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 24u);  // keyint 12: frame 24 is a real keyframe, well past 0

    DecodeOptions options;
    options.testSpliceSeekOvershootFrames = 24;  // the splice's first seek lands on frame 24
    const LoopProbe probe = probeLoop(path, ref, 3, 3, options);
    ASSERT_TRUE(probe.opened);
    // Whichever path resolves it (the byte-seek fallback, or a restart when that also
    // fails), the frame after the seam must still be the clip's true start, not frame 24
    // onward — the corruption the reviewer's no_first_check mutant would let through.
    EXPECT_EQ(probe.badPix, 0);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.missing, 0);
}

// --- TD-32: an AVI container with an MPEG-4 ASP (mpeg4) stream and B-frames has no
// decode-order timestamp field, so its demuxer can only back-fill a presentation pts
// for some of the stream's packets (see VideoDecoder::open(), "unreliableFramePts_")
// and leaves the rest (typically the I/P pictures) AV_NOPTS_VALUE. Trusting the pts
// where the demuxer did supply one mixes decode-order values into a presentation-order
// sequence: on the pre-fix code, looping this fixture corrupts most of the stream
// (badPix/badOrder nonzero) whether or not the Loop splice itself runs, because the
// same mixed-up pts poisons an ordinary restart-and-reseek too. The fix makes the
// decoder ignore this stream's pts entirely and fall back to its existing sequential
// (no-pts) timing, which is already correct since libavcodec's own B-frame reordering
// guarantees decode-output order is display order. ---

TEST(LoopSpliceSafety, Mpeg4AviBFramesDecodeCorrectlyAcrossLoopSeams) {
    const fs::path path = fixture("mpeg4_bframes.avi");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    // Start exactly at the clip's first frame (a real keyframe): seeking to an
    // arbitrary mid-file time in this codec/container pair is not frame-exact (the
    // same missing-pts problem this test fixes at the decode level also limits how
    // precisely a seek can land there) — a separate, pre-existing limitation this row
    // does not claim to fix. What TD-32 is about, and what this checks, is that once
    // playing, every Loop seam decodes in the right order with the right pixels.
    const int n = static_cast<int>(ref.pts.size());
    const LoopProbe probe = probeLoop(path, ref, n, 3);
    ASSERT_TRUE(probe.opened);
    EXPECT_EQ(probe.badPix, 0);
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
    // This codec/container pair's pts cannot be trusted (see above), so
    // lastForwardPts_ never carries a value and the Loop splice never attempts to run
    // for it — every seam falls back to the ordinary restart-and-reseek, which the fix
    // above makes decode correctly.
    EXPECT_GT(probe.restartsDelta, 0u);
}

// --- TD-33: a QuickTime/MP4 clip trimmed by an edit list still declares a duration
// (the container's trimmed stream length), just a short one; VideoDecoder::open() used
// to trust any present, positive duration outright, so plan_.duration (and therefore
// the Loop wrap point) landed 1-2 frames before the clip's real end on every pass, not
// just the first. The fix cross-checks a mov/mp4-family container's declared duration
// against the observed last frame's end and widens it when the two disagree. ---

TEST(LoopSpliceSafety, EditListTrimmedMp4KeepsEveryFrameOfEveryLoopPass) {
    const fs::path path = fixture("h264_editlist_trim.mp4");
    ASSERT_TRUE(fs::exists(path)) << path;
    const Reference ref = decodeReference(path);
    ASSERT_GT(ref.pts.size(), 3u);

    const LoopProbe probe = probeLoop(path, ref, 3, 3);
    ASSERT_TRUE(probe.opened);
    EXPECT_EQ(probe.badPix, 0);
    // The pre-fix bug wraps 1-2 frames early every pass, which shows up here as the
    // decode order jumping backward to 0 before the reference's last frame is reached.
    EXPECT_EQ(probe.badOrder, 0);
    EXPECT_EQ(probe.badDue, 0);
    EXPECT_EQ(probe.missing, 0);
}
