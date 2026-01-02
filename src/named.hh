#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <typeinfo>

#include "format.hh"
#include "str.hh"

namespace automat {

// The base type for all things that can be named.
//
// Due to possibilty of this class present multiple times in the inheritance (diamond inheritance),
// this should probably be inherited virtually.
struct Named {
  virtual ~Named() = default;

  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }
};

}  // namespace automat
