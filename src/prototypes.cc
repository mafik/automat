// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "prototypes.hh"

#include "object.hh"

namespace automat {

std::vector<std::shared_ptr<Object>>& Prototypes() {
  static std::vector<std::shared_ptr<Object>> prototypes;
  return prototypes;
}

// TODO: when objects are registered (in the __attribute__((constructor)) functions), they may not
// be themselves constructed! This should be fixed - we should move object registration to another
// time.
void RegisterPrototype(std::shared_ptr<Object>&& prototype) {
  assert(prototype.get());
  Prototypes().emplace_back(std::move(prototype));
}

std::shared_ptr<Object>* FindPrototype(maf::StrView name) {
  for (std::shared_ptr<Object>& prototype : Prototypes()) {
    if (prototype->Name() == name) {
      return &prototype;
    }
  }
  return nullptr;
}

}  // namespace automat