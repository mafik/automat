#pragma once

#include <include/core/SkMatrix.h>

#include "log.hh"

namespace automat {

const Logger& operator<<(const Logger&, SkMatrix&);

}  // namespace automat