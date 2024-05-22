#include "run_button.hh"

#include "base.hh"
#include "gui_button.hh"
#include "gui_shape_widget.hh"
#include "location.hh"
#include "pointer.hh"
#include "svg.hh"

namespace automat::gui {

RunButton::RunButton(Location* parent, float radius)
    : Button(MakeShapeWidget(kPlayShape, 0xffffffff)),
      ToggleButton(),
      CircularButtonMixin(radius),
      location(parent) {}

void RunButton::Activate(Pointer&) {
  if (Filled()) {
    if (location->run_task.scheduled) {
      // TODO: cancel the task
    } else if (location->long_running != nullptr) {
      location->long_running->Cancel();
      location->long_running = nullptr;
    }
  } else {
    // This will destroy a potential object saved in the error so it shouldn't
    // be automatic. Maybe we could ClearError only if there is no object?
    // Otherwise we should ask the user.
    // TODO(after Errors are visualized): Ask the user.
    location->ClearError();
    location->ScheduleRun();
  }
}

bool RunButton::Filled() const {
  if (location == nullptr) {
    return false;
  }
  return location->run_task.scheduled || (location->long_running != nullptr);
}

}  // namespace automat::gui