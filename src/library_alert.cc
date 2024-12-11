// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_alert.hh"

#include "base.hh"
#include "library_macros.hh"

#ifdef _WIN32
#include <windows.h>

#include "win32_window.hh"
#include "window.hh"

#endif

namespace automat {

#if not defined(NDEBUG)  // temporarily disable when in release mode
DEFINE_PROTO(Alert);
#endif

Argument Alert::message_arg("message", Argument::kRequiresObject);

LongRunning* Alert::OnRun(Location& here) {
  auto message = message_arg.GetObject(here);
  if (message.ok) {
    string text = message.object->GetText();
    if (test_interceptor) {
      test_interceptor->push_back(text);
    } else {
#ifdef _WIN32
      auto& win32_window = dynamic_cast<Win32Window&>(*gui::window->os_window);
      MessageBox(win32_window.hwnd, text.data(), "Alert", MB_OK);
#else  // not Windows
      LOG << text;
#endif
    }
  }
  return nullptr;
}

}  // namespace automat