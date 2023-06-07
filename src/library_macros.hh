#pragma once

#include "prototypes.hh"

#define DEFINE_PROTO(type) \
  const type type::proto;  \
  __attribute__((constructor)) void Register##type() { RegisterPrototype(type::proto); }
