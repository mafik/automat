// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "str.hh"

#ifdef __linux__
#include <xcb/xcb.h>
#endif

namespace automat::library {

struct Window : public Object {
  maf::Str title = "";

#ifdef __linux__
  xcb_window_t xcb_window = 0;
#endif

  Window();

  std::string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  std::shared_ptr<gui::Widget> MakeWidget() override;
};

}  // namespace automat::library