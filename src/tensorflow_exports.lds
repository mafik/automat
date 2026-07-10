/* SPDX-FileCopyrightText: Copyright 2026 Automat Authors */
/* SPDX-License-Identifier: MIT */

/* Warning: coded with a stochastic parrot */

/* Additional exports for the embedded libtensorflow.so (src/tensorflow.py
   passes this to the library's link). Upstream's version script hides the C++
   API; these patterns return the tensorflow, tsl and absl symbols automat's
   trampolines bind (tensorflow_trampolines.cpp) to the dynamic table so dlsym
   finds them. The length-prefixed patterns match the mangled namespace
   component (Itanium "10tensorflow", "3tsl", "4absl"), not a bare substring. */
automat_cpp_api {
  global:
    *10tensorflow*;
    *3tsl*;
    *4absl*;
};
