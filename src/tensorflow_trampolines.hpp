#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// The Linux trampolines that let automat call the embedded libtensorflow.so
// without linking it; see tensorflow_trampolines.cpp. Including this header
// from tensorflow_runtime.cpp pulls that translation unit into the automat
// link (the build wires a source in when its header is included).

// Points each trampoline at the address dlsym returns from the loaded library;
// returns false if any symbol is missing. Defined only on Linux.
extern "C" bool tf_bind_symbols(void* handle);
