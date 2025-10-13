// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "object.hh"

// Module for managing the object lifetime

namespace automat {

struct LifetimeObserver {
  Object& object;
  LifetimeObserver *prev, *next;

  LifetimeObserver(Object&);
  virtual ~LifetimeObserver();

  // This may be called from arbitrary thread. And will be called exactly once.
  virtual void OnDestroy() = 0;

  // Objects should call this function at the start of their destructor.
  //
  // This allows observers to access their data, while it's still valid.
  static void NotifyDestroy(Object&);

  static void CheckDestroyNotified(Object&);
};

}  // namespace automat
