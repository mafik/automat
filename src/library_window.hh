// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "str.hh"

#ifdef __linux__
#include <xcb/xcb.h>
#endif

namespace automat::library {

struct Window : public Object, Runnable {
  maf::Str title = "";

#ifdef __linux__
  xcb_window_t xcb_window = XCB_WINDOW_NONE;
#endif

  float x_min_ratio = 0.25f;
  float x_max_ratio = 0.75f;
  float y_min_ratio = 0.25f;
  float y_max_ratio = 0.75f;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  Ptr<gui::Widget> MakeWidget() override;

  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here) override;
};

}  // namespace automat::library