#include "prototypes.h"

namespace automat {

std::vector<const Object *> &Prototypes() {
  static std::vector<const Object *> prototypes;
  return prototypes;
}

void RegisterPrototype(const Object &prototype) {
  Prototypes().push_back(&prototype);
}

} // namespace automat