#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <array>

#include "base.hpp"

namespace automat::library {

extern constinit std::array<BasicCommand::Table, 10> kDigits;
extern constinit BasicCommand::Table kDot;
extern constinit BasicCommand::Table kBackspace;

struct Number : Object {
  double value;
  Number(double x = 0);
  Number(const Number&);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  string GetText() const override;
  void SetText(string_view text) override;

  DEF_INTERFACE(Number, Text, text, "Value")
  Str OnGet() { return obj->GetText(); }
  void OnSet(StrView);
  DEF_END(text);
  INTERFACES(text_tbl, kDigits[0], kDigits[1], kDigits[2], kDigits[3], kDigits[4], kDigits[5],
             kDigits[6], kDigits[7], kDigits[8], kDigits[9], kDot, kBackspace);

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
