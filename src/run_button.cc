#include "run_button.h"

#include "base.h"
#include "gui_shape_widget.h"
#include "location.h"
#include "log.h"

namespace automat::gui {

constexpr char kPlayShape[] =
    "M-5-8C-5.8-6-5.7 6-5 8-3 7.7 7.5 1.5 9 0 7.5-1.5-3-7.7-5-8Z";

RunButton::RunButton(Location *parent)
    : Button(parent, MakeShapeWidget(kPlayShape, 0xffffffff)),
      location(parent) {}

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

} // namespace automat::gui