#pragma once

// Class for working with paths. Based on python's pathlib.

#include "status.hh"
#include "str.hh"

namespace maf {

struct Path {
#if defined(_WIN32)
  constexpr static char kSeparator = '\\';
#else
  constexpr static char kSeparator = '/';
#endif

  Str str;

  Path(const char* str) : str(str) {}
  Path(Str str) : str(std::move(str)) {}
  Path(StrView path) : str(path) {}
  Path() = default;
  Path(const Path& other) = default;

  // Path to the currently executing binary.
  static Path ExecutablePath();

  static Path TempDirPath();

  Path Parent() const;

  // Replace initial "~" or "~user" with user's home directory.
  Path ExpandUser() const;

  // Follow symlink.
  Path ReadLink(Status&) const;

  void Unlink(Status&, bool missing_ok = false) const;

  void Rename(const Path& to, Status&) const;

  // Final path component.
  Str Name() const;

  // Final path component without suffix.
  Str Stem() const;

  Path WithStem(StrView stem) const;

  Path operator/(StrView rhs) const;

  operator Str() const { return str; }
  operator StrView() const { return str; }
  operator const char*() const { return str.c_str(); }
  const char* c_str() const { return str.c_str(); }
};

}  // namespace maf