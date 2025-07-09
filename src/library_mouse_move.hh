// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct MouseMove : Object {
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  Ptr<gui::Widget> MakeWidget() override;
};

}  // namespace automat::library