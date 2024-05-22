#include "library_alert.hh"

#include "base.hh"
#include "library_macros.hh"

#ifdef _WIN32
#include <windows.h>

#include "win_main.hh"
#endif

namespace automat {

#if not defined(NDEBUG)  // temporarily disable when in release mode
DEFINE_PROTO(Alert);
#endif

Argument Alert::message_arg("message", Argument::kRequiresObject);

#ifdef _WIN32
static void ShowAlert(string_view message) {
  MessageBox(main_window, message.data(), "Alert", MB_OK);
}
#else  // not Windows
static void ShowAlert(string_view message) { LOG << message; }
#endif

LongRunning* Alert::OnRun(Location& here) {
  auto message = message_arg.GetObject(here);
  if (message.ok) {
    string text = message.object->GetText();
    if (test_interceptor) {
      test_interceptor->push_back(text);
    } else {
      ShowAlert(text);
    }
  }
  return nullptr;
}

}  // namespace automat