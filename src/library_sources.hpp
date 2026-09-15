#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"

namespace automat::library {

// An object that displays a texture showing the source files embedded in the Automat binary.
// Provides a menu option to extract all embedded files to the local filesystem.
struct Sources : Object {
  Sources();

  DEF_INTERFACE(Sources, Command, extract_files, "Extract Files")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&);
  DEF_END(extract_files);

  INTERFACES(extract_files)

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
