#include "thread_name.h"

// clang-format off
#include <windows.h>
#include <processthreadsapi.h>
// clang-format on
#include <src/base/SkUTF.h>

#include <cassert>

void SetThreadName(std::string_view utf8) {
  int codepoints = SkUTF::CountUTF8(utf8.data(), utf8.size()) + 1;
  uint16_t utf16[codepoints];
  SkUTF::UTF8ToUTF16(utf16, codepoints, utf8.data(), utf8.size());
  utf16[codepoints - 1] = 0;
  SetThreadDescription(GetCurrentThread(), (wchar_t*)utf16);
}