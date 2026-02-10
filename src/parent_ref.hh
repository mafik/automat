#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Macro for getting a reference to the parent from a nested struct.
//
// Avoids 8B pointer to the parent using offsetof.
#define PARENT_REF(PARENT_T, FIELD)                                                     \
  struct PARENT_T& PARENT_T() {                                                         \
    return *reinterpret_cast<struct PARENT_T*>(reinterpret_cast<intptr_t>(this) -       \
                                               offsetof(struct PARENT_T, FIELD));       \
  }                                                                                     \
  const struct PARENT_T& PARENT_T() const {                                             \
    return *reinterpret_cast<const struct PARENT_T*>(reinterpret_cast<intptr_t>(this) - \
                                                     offsetof(struct PARENT_T, FIELD)); \
  }
