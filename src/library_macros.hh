// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "prototypes.hh"  // IWYU pragma: export

#define DEFINE_PROTO(type)                                  \
  type* type::proto;                                        \
  __attribute__((constructor)) void Register##type() {      \
    std::shared_ptr<Object> obj = std::make_shared<type>(); \
    type::proto = dynamic_cast<type*>(obj.get());           \
    assert(type::proto);                                    \
    RegisterPrototype(std::move(obj));                      \
  }
