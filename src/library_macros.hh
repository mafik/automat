// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "prototypes.hh"  // IWYU pragma: export

#define DEFINE_PROTO(type)                             \
  std::shared_ptr<type> type::proto;                   \
  __attribute__((constructor)) void Register##type() { \
    type::proto = std::make_shared<type>();            \
    RegisterPrototype(type::proto);                    \
  }
