#pragma once

#include <include/core/SkMatrix.h>

#include "log.h"

namespace automat {

const Logger &operator<<(const Logger &, SkMatrix &);

} // namespace automat