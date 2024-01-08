#include "run_button.hh"

#include "base.hh"
#include "gui_shape_widget.hh"
#include "location.hh"
#include "log.hh"

namespace automat::gui {

constexpr char kPlayShape[] = "M-5-8C-5.8-6-5.7 6-5 8-3 7.7 7.5 1.5 9 0 7.5-1.5-3-7.7-5-8Z";
constexpr char kNextShape[] =
    "M-7-8C-7.8-6-7.7 6-7 8-5 7.7 5.5 1.5 7 0Q7-4 6-7.5L8-8Q9-4 9 0 9 4 8 8L6 7.5Q7 4 7 "
    "0C5.5-1.5-5-7.7-7-8Z";

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