#pragma once

#include <vector>

namespace automaton {

struct Object;

std::vector<const Object *> &Prototypes();
void RegisterPrototype(const Object &prototype);

} // namespace automaton