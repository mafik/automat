// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPath.h>

#include "math.hh"

namespace automat {

std::optional<Vec2> Raycast(const SkPath& path, const Vec2AndDir& ray);

}  // namespace automat