// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <vector>

#include "str.hh"

namespace automat {

struct Object;

std::vector<std::shared_ptr<Object>>& Prototypes();
void RegisterPrototype(std::shared_ptr<Object> prototype);
std::shared_ptr<Object> FindPrototype(maf::StrView name);

}  // namespace automat