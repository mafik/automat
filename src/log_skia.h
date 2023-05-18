#pragma once

#include <include/core/SkMatrix.h>

#include "log.h"

namespace automaton {

const Logger &operator<<(const Logger &, SkMatrix &);

} // namespace automaton