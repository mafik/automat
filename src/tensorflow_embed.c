// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "tensorflow_embed.h"

const unsigned char tf_library[] = {
#ifdef _WIN32
#embed <tensorflow.dll>
#else
#embed <libtensorflow.so>
#endif
};
const size_t tf_library_size = sizeof(tf_library);
