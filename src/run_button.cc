#include "run_button.hh"

#include "base.hh"
#include "gui_shape_widget.hh"
#include "location.hh"
#include "svg.hh"

namespace automat::gui {

RunButton::RunButton(Location* parent)
    : Button(MakeShapeWidget(kPlayShape, 0xffffffff)), location(parent) {}

void RunButton::Activate() {
  if (Filled()) {
    // TODO: Cancel.
  } else {
    // This will destroy a potential object saved in the error so it shouldn't
    // be automatic. Maybe we could ClearError only if there is no object?
    // Otherwise we should ask the user.
    // TODO(after Errors are visualized): Ask the user.
    location->ClearError();
    location->ScheduleRun();
  }
}

bool RunButton::Filled() const { return location->run_task.scheduled; }

}  // namespace automat::gui