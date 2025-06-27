// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "thread_name.hh"

#include <tracy/common/TracySystem.hpp>

void SetThreadName(std::string_view utf8, int group_hint) {
  tracy::SetThreadNameWithHint(utf8.data(), group_hint);
}
