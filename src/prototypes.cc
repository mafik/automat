#include "prototypes.hh"

#include "object.hh"

namespace automat {

std::vector<const Object*>& Prototypes() {
  static std::vector<const Object*> prototypes;
  return prototypes;
}

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