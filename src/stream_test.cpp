// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "stream.hpp"

#include "gtest.hpp"

using namespace automat;

TEST(RateEstimatorTest, SteadyRate) {
  RateEstimator e;
  uint64_t total = 0;
  double rate = 0;
  for (int i = 0; i <= 20; ++i) {
    rate = e.Update(i * 0.2, total);
    total += 200;  // 1000 bytes per second
  }
  EXPECT_NEAR(rate, 1000, 1);
}

TEST(RateEstimatorTest, StallFallsToZero) {
  RateEstimator e;
  for (int i = 0; i <= 10; ++i) e.Update(i * 0.2, i * 200);
  // The producer stalls; the UI keeps sampling every frame. Once the whole
  // ring is inside the stall, the reported rate reaches zero.
  double stalled = 1e9;
  for (int i = 1; i <= 40; ++i) stalled = e.Update(2.0 + i * 0.1, 2000);
  EXPECT_NEAR(stalled, 0, 1);
}

TEST(RateEstimatorTest, CounterResetRestartsCleanly) {
  RateEstimator e;
  for (int i = 0; i <= 10; ++i) e.Update(i * 0.2, i * 1000);
  e.Update(2.4, 100);  // process restarted; totals dropped
  double rate = e.Update(2.6, 200);
  EXPECT_GE(rate, 0);
  EXPECT_LT(rate, 2000);
}

TEST(RateEstimatorTest, SubIntervalSamplesAreCoalesced) {
  RateEstimator e;
  e.Update(0, 0);
  for (int i = 1; i <= 100; ++i) e.Update(0.001 * i, i * 10);
  EXPECT_LE(e.count, 2);
}

TEST(FormatBytesPerSecondTest, Units) {
  EXPECT_EQ(FormatBytesPerSecond(0), "0 B/s");
  EXPECT_EQ(FormatBytesPerSecond(512), "512 B/s");
  EXPECT_EQ(FormatBytesPerSecond(2048), "2.0 kB/s");
  EXPECT_EQ(FormatBytesPerSecond(3.5 * 1024 * 1024), "3.5 MB/s");
}

TEST(FormatBytesTest, Units) {
  EXPECT_EQ(FormatBytes(0), "0 B");
  EXPECT_EQ(FormatBytes(65536), "64 KiB");
  EXPECT_EQ(FormatBytes(62823), "61.4 KiB");
  EXPECT_EQ(FormatBytes(3 * 1024 * 1024), "3.0 MiB");
}
