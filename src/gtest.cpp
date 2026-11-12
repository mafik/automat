// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "gtest.hpp"

#if defined(__linux__)
#include "crt.hpp"
#include "log.hpp"
#endif

int main(int argc, char** argv) {
#if defined(__linux__)
  using namespace automat;
  Status crt_status;
  crt::Init(crt_status);
  if (!OK(crt_status)) {
    ERROR << "Couldn't initialize the host libc: " << crt_status;
    return 1;
  }
#endif
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
