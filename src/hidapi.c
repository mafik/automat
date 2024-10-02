// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#if defined(_WIN32)

#pragma comment(lib, "hid")

// clang-format off
#include <wtypes.h>
#include <winioctl.h>
// clang-format on

#include "win_hidapi.c"

#elif defined(__linux__)

#include "linux_hidapi.c"

#endif
