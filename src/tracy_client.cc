// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

// On Linux we use -DTRACY_LIBUNWIND_BACKTRACE (tracy.py)
// This include ensures that libunwind is going to be linked into the binary
#ifdef __linux__
#include <libunwind.h>
#endif

//  Compile TracyClient.cpp using the same flags as the rest of the project
#include <TracyClient.cpp>
