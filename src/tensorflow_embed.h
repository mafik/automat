#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// The embedded TensorFlow shared library (installed by src/tensorflow.py):
// libtensorflow.so on Linux, tensorflow.dll on Windows. The #embed lives in
// a C translation unit because clang only has a fast path for it in C; in C++
// it materializes the initializer per element and runs out of memory on a
// library this large.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char tf_library[];
extern const size_t tf_library_size;

#ifdef __cplusplus
}
#endif
