// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <typeindex>
#include <unordered_map>

#include "optional.hpp"
#include "ptr.hpp"
#include "str.hpp"
#include "string_multimap.hpp"
#include "vec.hpp"

namespace automat {

struct Object;

struct PrototypeLibrary {
  PrototypeLibrary();

  Object* Find(const std::type_info&);
  Object* Find(StrView name);

  template <typename T>
  T* Find() {
    return dynamic_cast<T*>(Find(typeid(T)));
  }

  std::unordered_map<std::type_index, Ptr<Object>> type_index;
  string_map<Ptr<Object>> name_index;
  Vec<Ptr<Object>> default_toolbar;
};

extern Optional<PrototypeLibrary> prototypes;

}  // namespace automat