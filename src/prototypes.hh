// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <typeindex>
#include <unordered_map>

#include "optional.hh"
#include "ptr.hh"
#include "str.hh"
#include "string_multimap.hh"
#include "vec.hh"

namespace automat {

struct Object;

struct PrototypeLibrary {
  PrototypeLibrary();
  ~PrototypeLibrary();

  Object* Find(const std::type_info&);
  Object* Find(maf::StrView name);

  template <typename T>
  T* Find() {
    return dynamic_cast<T*>(Find(typeid(T)));
  }

  std::unordered_map<std::type_index, Ptr<Object>> type_index;
  string_map<Ptr<Object>> name_index;
  maf::Vec<Ptr<Object>> default_toolbar;
};

extern maf::Optional<PrototypeLibrary> prototypes;

}  // namespace automat