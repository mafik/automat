// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_alert.hh"

#include "base.hh"

#ifdef _WIN32
#include <windows.h>

#include "root_widget.hh"
#include "win32_window.hh"

#endif

namespace automat {

Argument Alert::message_arg("message", Argument::kRequiresObject);

void Alert::OnRun(Location& here, RunTask&) {
  auto message = message_arg.GetObject(here);
  if (message.ok) {
    string text = message.object->GetText();
    if (test_interceptor) {
      test_interceptor->push_back(text);
    } else {
#ifdef _WIN32
      auto& win32_window = dynamic_cast<Win32Window&>(*gui::root_widget->window);
      MessageBox(win32_window.hwnd, text.data(), "Alert", MB_OK);
#else  // not Windows
      LOG << text;
#endif
    }
  }
}

}  // namespace automat