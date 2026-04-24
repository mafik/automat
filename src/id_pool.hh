// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <mutex>

#include "int.hh"
#include "vec.hh"

namespace automat {

struct IdPool {
  std::mutex mutex;

  I64 min;
  I64 max;
  I64 next;
  Vec<I64> free_list;

  IdPool(I64 min, I64 max) : min(min), max(max), next(min) {}

  struct Handle {
    IdPool* pool = nullptr;
    I64 id;

    Handle(IdPool& pool) {
      std::lock_guard lock(pool.mutex);
      if (!pool.free_list.empty()) {
        id = pool.free_list.back();
        pool.free_list.pop_back();
      } else {
        assert(pool.next < pool.max);
        id = pool.next++;
      }
      this->pool = &pool;
    }

    Handle(Handle&& that) : pool(that.pool), id(that.id) { that.pool = nullptr; }
    Handle& operator=(Handle&& that) {
      if (this != &that) {
        Release();
        pool = that.pool;
        id = that.id;
        that.pool = nullptr;
      }
      return *this;
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    void Release() {
      if (pool) {
        std::lock_guard lock(pool->mutex);
        pool->free_list.InsertSorted(id, id);
        while (!pool->free_list.empty() && pool->free_list.back() == pool->next - 1) {
          pool->free_list.pop_back();
          pool->next--;
        }
        pool = nullptr;
      }
    }

    ~Handle() { Release(); }
  };

  Handle Acquire() { return Handle(*this); }
};

}  // namespace automat