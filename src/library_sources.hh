// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

// An object that displays a texture showing the source files embedded in the Automat binary.
// Provides a menu option to extract all embedded files to the local filesystem.
struct Sources : Object {
  Sources();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, ReferenceCounted&) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
