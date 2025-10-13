// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "object_lifetime.hh"

#include <mutex>
#include <unordered_set>

namespace automat {

// Mutex that guards the access to the object observers
std::recursive_mutex object_observers_mutex;

struct HashLifetimeObserver {
  using is_transparent = std::true_type;

  size_t operator()(const LifetimeObserver* observer) const {
    return std::hash<const void*>{}(&observer->object);
  }

  size_t operator()(const Object* object) const { return std::hash<const void*>{}(object); }
};

struct EqualLifetimeObserver {
  using is_transparent = std::true_type;

  bool operator()(const LifetimeObserver* lhs, const LifetimeObserver* rhs) const {
    return &lhs->object == &rhs->object;
  }

  bool operator()(const Object* lhs, const LifetimeObserver* rhs) const {
    return lhs == &rhs->object;
  }
};

std::unordered_set<LifetimeObserver*, HashLifetimeObserver, EqualLifetimeObserver> object_observers;

LifetimeObserver::LifetimeObserver(Object& object) : object(object), prev(nullptr), next(nullptr) {
  auto lock = std::lock_guard(object_observers_mutex);

  if (auto existing_observer_it = object_observers.find(this);
      existing_observer_it != object_observers.end()) {
    next = *existing_observer_it;
    next->prev = this;
    object_observers.erase(existing_observer_it);
  }
  object_observers.insert(this);
}

LifetimeObserver::~LifetimeObserver() {
  auto lock = std::lock_guard(object_observers_mutex);
  if (prev == nullptr) {  // we're the head of the list
    object_observers.erase(this);
    if (next) {
      object_observers.insert(next);
      next->prev = nullptr;
    }
  } else if (prev == this) {
    // this object was already destroyed and this LifetimeObserver is not part of any list
  } else {  // we're somewhere in the middle or at the end
    if (next) {
      next->prev = prev;
    }
    prev->next = next;
  }
}

void LifetimeObserver::NotifyDestroy(Object& object) {
  auto lock = std::lock_guard(object_observers_mutex);
  if (auto observer_it = object_observers.find(&object); observer_it != object_observers.end()) {
    LifetimeObserver* head = *observer_it;
    object_observers.erase(observer_it);
    do {  // no null check because we never insert null
      auto* next = head->next;
      head->prev = head;
      head->next = head;
      head->OnDestroy();
      head = next;
    } while (head);
  }
}

void LifetimeObserver::CheckDestroyNotified(Object& object) {
  auto lock = std::lock_guard(object_observers_mutex);
  if (auto observer_it = object_observers.find(&object); observer_it != object_observers.end()) {
    ERROR << "Object didn't call LifetimeObserver::NotifyDestroy in its destructor!";
    // When NotifyDestry is called like that, it runs after the contents of the destroyed objects
    // are in invalid state.
    NotifyDestroy(object);
  }
}

}  // namespace automat
