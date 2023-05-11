#include "library_alert.h"

#include "library_macros.h"

#ifdef _WIN32
#include <windows.h>
#include "win_main.h"
#endif

namespace automaton {

DEFINE_PROTO(Alert);
Argument Alert::message_arg("message", Argument::kRequiresObject);

#ifdef _WIN32
static void ShowAlert(string_view message) {
  MessageBox(main_window, message.data(), "Alert", MB_OK);
}
#else // not Windows
static void ShowAlert(string_view message) {
  LOG() << text;
}
#endif

void Alert::Run(Location &here) {
  auto message = message_arg.GetObject(here);
  if (message.ok) {
    string text = message.object->GetText();
    alerts_for_tests.push_back(text);
    ShowAlert(text);
  }
}

} // namespace automaton