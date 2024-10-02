// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "thread_name.hh"

#if defined(_WIN32)

// clang-format off
#include <windows.h>
#include <processthreadsapi.h>
// clang-format on

#include <src/base/SkUTF.h>

void SetThreadName(std::string_view utf8) {
  int codepoints = SkUTF::CountUTF8(utf8.data(), utf8.size()) + 1;
  uint16_t utf16[codepoints];
  SkUTF::UTF8ToUTF16(utf16, codepoints, utf8.data(), utf8.size());
  utf16[codepoints - 1] = 0;
  SetThreadDescription(GetCurrentThread(), (wchar_t*)utf16);
}

#else  // defined(_WIN32)

#include <pthread.h>

void SetThreadName(std::string_view utf8) { pthread_setname_np(pthread_self(), utf8.data()); }

#endif  // defined(_WIN32)