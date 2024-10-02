// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#if defined(_WIN32)

#pragma comment(lib, "hid")

#include <winioctl.h>
#include <wtypes.h>

#include "win_hidapi.c"


#elif defined(__linux__)

#include "linux_hidapi.c"

#endif
