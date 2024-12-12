// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>

#include "optional.hh"
#include "str.hh"
#include "string_multimap.hh"

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

  std::unordered_map<std::type_index, std::shared_ptr<Object>> type_index;
  string_map<std::shared_ptr<Object>> name_index;
};

extern maf::Optional<PrototypeLibrary> prototypes;

}  // namespace automat