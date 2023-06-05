#pragma once

#include <vector>

namespace automat {

struct Object;

std::vector<const Object*>& Prototypes();
void RegisterPrototype(const Object& prototype);

}  // namespace automat