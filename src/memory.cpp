// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "memory.hpp"

#if defined(__linux__)

#include "status.hpp"
#include "virtual_fs.hpp"

extern "C" char __executable_start[];
extern "C" char _end[];

namespace automat {

bool InMainExecutable(intptr_t address) {
  return address >= (intptr_t)__executable_start && address < (intptr_t)_end;
}

MemoryMap MemoryMap::SnapshotSelf() {
  MemoryMap memory_map;
  Status status_ignored;
  memory_map.proc_self_maps = fs::real.Read("/proc/self/maps", status_ignored);
  return memory_map;
}

namespace {

bool ParseHex(const char*& line, char terminator, uintptr_t& out) {
  out = 0;
  auto start = line;
  while (true) {
    char c = *line;
    if (c >= '0' && c <= '9') {
      out = (out << 4) | (c - '0');
    } else if (c >= 'a' && c <= 'f') {
      out = (out << 4) | (c - 'a' + 10);
    } else {
      return c == terminator && line++ != start;
    }
    ++line;
  }
}

StrView TakeField(const char*& line) {
  auto start = line;
  while (*line != '\0' && *line != ' ' && *line != '\n') ++line;
  StrView field(start, line - start);
  while (*line == ' ') ++line;
  return field;
}

StrView TakeTail(const char*& line) {
  auto start = line;
  while (*line != '\0' && *line != '\n') ++line;
  return StrView(start, line - start);
}

}  // namespace

void MemoryMap::iterator::ParseNextEntry() {
  const char* line = next_unparsed;
  while (*line == '\n') ++line;
  if (*line == '\0') {
    next_unparsed = nullptr;
    return;
  }

  bool ok = ParseHex(line, '-', current.address_start) &&
            ParseHex(line, ' ', current.address_end) && !TakeField(line).empty() &&
            ParseHex(line, ' ', current.file_offset) && !TakeField(line).empty();
  if (!ok) {
    next_unparsed = nullptr;
    return;
  }
  StrView inode = TakeField(line);
  StrView tail = TakeTail(line);
  current.file_path = inode == "0" ? StrView() : tail;
  next_unparsed = line;
}

void ResolveAddress(intptr_t addr, Str& out_filename, intptr_t& out_offset) {
  for (auto& entry : MemoryMap::SnapshotSelf()) {
    if (addr >= entry.address_start && addr < entry.address_end) {
      out_filename = entry.file_path;
      out_offset = (addr - entry.address_start) + entry.file_offset;
      return;
    }
  }
  out_filename = "";
  out_offset = addr;
}

}  // namespace automat

#elif defined(_WIN32)

#include "win32.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace automat {

bool InMainExecutable(intptr_t address) {
  auto begin = (intptr_t)&__ImageBase;
  auto* nt_headers = (IMAGE_NT_HEADERS*)((char*)begin + __ImageBase.e_lfanew);
  return address >= begin && address < begin + nt_headers->OptionalHeader.SizeOfImage;
}

void ResolveAddress(intptr_t addr, Str& out_filename, intptr_t& out_offset) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)addr, &module)) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(module, path, MAX_PATH)) {
      out_filename = win32::WideToUtf8(path);
      out_offset = addr - (intptr_t)module;
      return;
    }
  }
  out_filename = "";
  out_offset = addr;
}

}  // namespace automat

#endif
