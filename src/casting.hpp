// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/Support/Casting.h>

namespace automat {
using llvm::cast;
using llvm::cast_if_present;
using llvm::dyn_cast;
using llvm::dyn_cast_if_present;
using llvm::isa;
}  // namespace automat
