// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "prototypes.hh"

#include "object.hh"

namespace automat {

std::vector<const Object*>& Prototypes() {
  static std::vector<const Object*> prototypes;
  return prototypes;
}

// TODO: when objects are registered (in the __attribute__((constructor)) functions), they may not
// be themselves constructed! This should be fixed - we should move object registration to another
// time.
void RegisterPrototype(const Object& prototype) { Prototypes().push_back(&prototype); }

const Object* FindPrototype(maf::StrView name) {
  for (const Object* prototype : Prototypes()) {
    if (prototype->Name() == name) {
      return prototype;
    }
  }
  return nullptr;
}

}  // namespace automat