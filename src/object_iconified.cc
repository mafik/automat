// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "object_iconified.hh"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "base.hh"
#include "object_lifetime.hh"

namespace automat {

struct IconifiedCleaner;

std::shared_mutex iconified_objects_mutex;
std::unordered_map<Object*, IconifiedCleaner> iconified_objects;

struct IconifiedCleaner : LifetimeObserver {
  IconifiedCleaner(Object& object) : LifetimeObserver(object) {}
  void OnDestroy() override { SetIconified(object, false); }
};

bool IsIconified(Object* object) {
  auto lock = std::shared_lock(iconified_objects_mutex);
  return iconified_objects.find(object) != iconified_objects.end();
}

void Iconify(Object& object) {
  auto lock = std::unique_lock(iconified_objects_mutex);
  iconified_objects.emplace(&object, object);
  object.here->InvalidateConnectionWidgets(false, false);
}

void Deiconify(Object& object) {
  auto lock = std::unique_lock(iconified_objects_mutex);
  iconified_objects.erase(&object);
  object.here->InvalidateConnectionWidgets(false, false);
}

void SetIconified(Object& object, bool iconified) {
  if (iconified) {
    Iconify(object);
  } else {
    Deiconify(object);
  }
}

}  // namespace automat
