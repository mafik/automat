#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "str.hpp"

namespace automat {

#if defined(__linux__)
// Utility for iterating over process memory maps
struct MemoryMap {
  Str proc_self_maps;
  static MemoryMap SnapshotSelf();

  struct end_iterator {};

  struct Entry {
    uintptr_t address_start;
    uintptr_t address_end;
    StrView file_path;
    uintptr_t file_offset;
  };

  struct iterator {
    Entry current;
    const char* next_unparsed;

    void ParseNextEntry();

    iterator(MemoryMap& memory_map) : next_unparsed(memory_map.proc_self_maps.data()) {
      ParseNextEntry();
    }

    iterator& operator++() {
      ParseNextEntry();
      return *this;
    }
    const Entry& operator*() { return current; }
    bool operator==(end_iterator) { return next_unparsed == 0; }
  };

  iterator begin() { return iterator(*this); }
  end_iterator end() { return {}; }
};
#endif

bool InMainExecutable(intptr_t address);

// Find filename & offset where the addr is mapped to.
//
// Returns empty out_filename if the address is not mapped to file.
void ResolveAddress(intptr_t addr, Str& out_filename, intptr_t& out_offset);

}  // namespace automat
