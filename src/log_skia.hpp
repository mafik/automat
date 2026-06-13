// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkMatrix.h>

#include "log.hpp"

namespace automat {

const LogEntry& operator<<(const LogEntry&, const SkMatrix&);

}  // namespace automat
