// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "memory.hpp"

#include <vector>

#include "gtest.hpp"

using namespace automat;

#if defined(__linux__)
TEST(MemoryMap, ParsesProcSelfMaps) {
  MemoryMap map;
  map.proc_self_maps =
      "00400000-00452000 r-xp 00000000 08:02 173521                     /usr/bin/dbus daemon\n"
      "7f5b1c000000-7f5b1c021000 rw-p 00000000 00:00 0\n"
      "7ffc9a1e2000-7ffc9a203000 rw-p 00000000 00:00 0                  [stack]\n"
      "7f5b1d4a0000-7f5b1d4c2000 r--p 00021000 fd:01 1052               /usr/lib/libc.so.6 "
      "(deleted)";
  std::vector<MemoryMap::Entry> entries;
  for (auto& entry : map) {
    entries.push_back(entry);
  }
  ASSERT_EQ(entries.size(), 4);
  EXPECT_EQ(entries[0].address_start, 0x400000u);
  EXPECT_EQ(entries[0].address_end, 0x452000u);
  EXPECT_EQ(entries[0].file_offset, 0u);
  EXPECT_EQ(entries[0].file_path, "/usr/bin/dbus daemon");
  EXPECT_EQ(entries[1].address_start, 0x7f5b1c000000u);
  EXPECT_EQ(entries[1].file_path, "");
  EXPECT_EQ(entries[2].file_path, "");
  EXPECT_EQ(entries[3].address_end, 0x7f5b1d4c2000u);
  EXPECT_EQ(entries[3].file_offset, 0x21000u);
  EXPECT_EQ(entries[3].file_path, "/usr/lib/libc.so.6 (deleted)");
}

TEST(MemoryMap, EmptyMapHasNoEntries) {
  MemoryMap map;
  EXPECT_TRUE(map.begin() == map.end());
}
#endif

TEST(InMainExecutable, DetectsOwnImage) {
  int local;
  EXPECT_TRUE(InMainExecutable((intptr_t)&InMainExecutable));
  EXPECT_FALSE(InMainExecutable((intptr_t)&local));
}

TEST(ResolveAddress, ResolvesOwnCode) {
  Str file;
  intptr_t offset;
  ResolveAddress((intptr_t)&ResolveAddress, file, offset);
  EXPECT_FALSE(file.empty());
  EXPECT_NE(offset, (intptr_t)&ResolveAddress);
}
