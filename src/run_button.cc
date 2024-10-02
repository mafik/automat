// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "run_button.hh"

#include <include/core/SkColor.h>

#include "base.hh"
#include "gui_button.hh"
#include "location.hh"
#include "pointer.hh"
#include "svg.hh"

using namespace std;

namespace automat::gui {

RunButton::RunButton(Location* parent, float radius)
    : location(parent),
      ToggleButton(
          make_unique<ColoredButton>(
              kPlayShape, ColoredButtonArgs{.fg = SK_ColorBLACK,
                                            .bg = SK_ColorWHITE,
                                            .on_click =
                                                [this](gui::Pointer& ptr) {
                                                  if (location->run_task.scheduled) {
                                                    // TODO: cancel the task
                                                  } else if (location->long_running != nullptr) {
                                                    location->long_running->Cancel();
                                                    location->long_running = nullptr;
                                                  }
                                                }}),
          make_unique<ColoredButton>(
              kPlayShape,
              ColoredButtonArgs{
                  .fg = SK_ColorWHITE, .bg = SK_ColorBLACK, .on_click = [this](gui::Pointer& ptr) {
                    // This will destroy a potential object saved in the error so it shouldn't
                    // be automatic. Maybe we could ClearError only if there is no object?
                    // Otherwise we should ask the user.
                    // TODO(after Errors are visualized): Ask the user.
                    location->ClearError();
                    location->ScheduleRun();
                  }})) {}

bool RunButton::Filled() const {
  if (location == nullptr) {
    return false;
  }
  return location->run_task.scheduled || (location->long_running != nullptr);
}

PowerButton::PowerButton(OnOff* target, SkColor fg, SkColor bg)
    : target(target),
      ToggleButton(
          make_unique<ColoredButton>(
              kPowerSVG,
              ColoredButtonArgs{
                  .fg = bg, .bg = fg, .on_click = [this](gui::Pointer& p) { Activate(p); }}),
          make_unique<ColoredButton>(
              kPowerSVG, ColoredButtonArgs{.fg = fg, .bg = bg, .on_click = [this](gui::Pointer& p) {
                                             Activate(p);
                                           }})) {}

void PowerButton::Activate(gui::Pointer& p) {
  target->Toggle();
  InvalidateDrawCache();
}
bool PowerButton::Filled() const { return target->IsOn(); }
}  // namespace automat::gui