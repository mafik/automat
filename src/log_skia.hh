// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkMatrix.h>

#include "log.hh"

namespace automat {

const LogEntry& operator<<(const LogEntry&, SkMatrix&);

}  // namespace automat