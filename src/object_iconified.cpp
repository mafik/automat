// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "object_iconified.hpp"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "base.hpp"
#include "object_lifetime.hpp"

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

static void InvalidateConnectionWidgetsEverywhere(Object& object) {
  auto vm_lock = std::lock_guard(vm.mutex);
  for (auto& board : vm.boards) {
    if (auto* loc = board->LocationOrNull(object)) {
      loc->InvalidateConnectionWidgets(false, false);
    }
  }
}

void Iconify(Object& object) {
  auto lock = std::unique_lock(iconified_objects_mutex);
  iconified_objects.emplace(&object, object);
  InvalidateConnectionWidgetsEverywhere(object);
}

void Deiconify(Object& object) {
  auto lock = std::unique_lock(iconified_objects_mutex);
  iconified_objects.erase(&object);
  InvalidateConnectionWidgetsEverywhere(object);
}

void SetIconified(Object& object, bool iconified) {
  if (iconified) {
    Iconify(object);
  } else {
    Deiconify(object);
  }
}

}  // namespace automat
