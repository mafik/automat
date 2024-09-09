#include "run_button.hh"

#include "base.hh"
#include "gui_button.hh"
#include "gui_shape_widget.hh"
#include "location.hh"
#include "pointer.hh"
#include "svg.hh"

namespace automat::gui {

RunButton::RunButton(Location* parent, float radius)
    : RunLocationMixin(parent),
      CircularButtonMixin(radius),
      child(MakeShapeWidget(kPlayShape, 0xffffffff)) {}

void RunLocationMixin::Activate(Pointer& p) {
  ToggleButton::Activate(p);
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

bool RunLocationMixin::Filled() const {
  if (location == nullptr) {
    return false;
  }
  return location->run_task.scheduled || (location->long_running != nullptr);
}

RunLocationMixin::RunLocationMixin(Location* parent) : Button(), location(parent) {}

PowerButton::PowerButton(OnOff* target)
    : RunOnOffMixin(target), child(MakeShapeWidget(kPowerSVG, SK_ColorWHITE)) {}

RunOnOffMixin::RunOnOffMixin(OnOff* target) : Button(), target(target) {}
}  // namespace automat::gui