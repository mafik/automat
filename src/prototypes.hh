// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <vector>

#include "str.hh"

namespace automat {

struct Object;

std::vector<const Object*>& Prototypes();
void RegisterPrototype(const Object& prototype);
const Object* FindPrototype(maf::StrView name);

}  // namespace automat