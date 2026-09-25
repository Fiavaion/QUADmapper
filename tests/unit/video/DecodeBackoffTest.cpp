// Decode-error backoff (QM-088): repeated non-EOF decode failures must not spin the
// decode thread; past a threshold it waits, doubling up to a cap.

#include <gtest/gtest.h>

#include "video/DecodeBackoff.hpp"

using namespace quadmapper;
using std::chrono::milliseconds;

TEST(DecodeBackoff, RetriesImmediatelyBelowThreshold) {
    DecodeBackoff backoff;
    for (int i = 1; i < DecodeBackoff::ERROR_THRESHOLD; ++i) {
        EXPECT_EQ(backoff.onFailure(), milliseconds(0)) << "failure " << i;
        EXPECT_FALSE(backoff.failing());
    }
}

TEST(DecodeBackoff, BacksOffExponentiallyUpToTheCap) {
    DecodeBackoff backoff;
    for (int i = 1; i < DecodeBackoff::ERROR_THRESHOLD; ++i) backoff.onFailure();

    EXPECT_EQ(backoff.onFailure(), DecodeBackoff::FIRST_DELAY);
    EXPECT_TRUE(backoff.failing());
    EXPECT_EQ(backoff.onFailure(), DecodeBackoff::FIRST_DELAY * 2);
    EXPECT_EQ(backoff.onFailure(), DecodeBackoff::FIRST_DELAY * 4);

    milliseconds last{0};
    for (int i = 0; i < 100; ++i) last = backoff.onFailure();
    EXPECT_EQ(last, DecodeBackoff::MAX_DELAY);
}

TEST(DecodeBackoff, SuccessResets) {
    DecodeBackoff backoff;
    for (int i = 0; i < DecodeBackoff::ERROR_THRESHOLD + 3; ++i) backoff.onFailure();
    ASSERT_TRUE(backoff.failing());

    backoff.onSuccess();
    EXPECT_FALSE(backoff.failing());
    EXPECT_EQ(backoff.onFailure(), milliseconds(0));
}
