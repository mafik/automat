// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "file_import.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "format.hpp"

namespace automat {

Path AutomatDir() { return Path::ExecutablePath().Parent(); }

Path CreateUniqueFile(const Path& dir, StrView basename, Status& status) {
  Path candidate = dir / basename;
  Str stem = Path(Str(basename)).Stem();
  Str ext = candidate.str.substr(stem.size() + (dir.str.size() + 1));  // trailing ".ext" or ""

  size_t digits = stem.size();
  while (digits > 0 && std::isdigit((unsigned char)stem[digits - 1])) --digits;
  long number = digits < stem.size() ? std::stol(stem.substr(digits)) : 1;
  Str prefix = stem.substr(0, digits);

  while (true) {
#if defined(_WIN32)
    HANDLE file = CreateFileA(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      return candidate;
    }
    if (GetLastError() != ERROR_FILE_EXISTS) {
      AppendErrorMessage(status) += f("CreateFile({}): error {}", candidate.str, GetLastError());
      return Path();
    }
#else
    int fd = open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd >= 0) {
      close(fd);
      return candidate;
    }
    if (errno != EEXIST) {
      AppendErrorMessage(status) += f("open({}): {}", candidate.str, strerror(errno));
      return Path();
    }
#endif
    ++number;
    candidate = dir / f("{}{}{}", prefix, number, ext);
  }
}

static Str UriDecode(StrView s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Str out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += (char)(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

static Str UriEncode(StrView s) {
  constexpr StrView kUnreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~/:";
  Str out;
  for (char c : s) {
    if (kUnreserved.find(c) != StrView::npos) {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

Path PathFromFileURI(StrView uri, Status& status) {
  constexpr StrView kScheme = "file://";
  if (!uri.starts_with(kScheme)) {
    return Path(UriDecode(uri));  // plain or relative path
  }
  StrView rest = uri.substr(kScheme.size());
  if (!rest.starts_with('/')) {
    // IDEA: it would be cool to support host-based file:// URLs
    AppendErrorMessage(status) += f("host-based file:// URLs are not supported: {}", uri);
    return Path();
  }
#if defined(_WIN32)
  Str path = UriDecode(rest.substr(1));
  std::replace(path.begin(), path.end(), '/', '\\');
  return Path(path);
#else
  return Path(UriDecode(rest));
#endif
}

Str FileURIFromPath(StrView path) {
#if defined(_WIN32)
  Str slashes(path);
  std::replace(slashes.begin(), slashes.end(), '\\', '/');
  return f("file:///{}", UriEncode(slashes));
#else
  return f("file://{}", UriEncode(path));
#endif
}

Str GetSerializablePath(StrView path) {
  Str dir = AutomatDir().str + Path::kSeparator;
  if (!path.starts_with(dir)) return FileURIFromPath(path);
  Str relative(path.substr(dir.size()));
#if defined(_WIN32)
  std::replace(relative.begin(), relative.end(), '\\', '/');
#endif
  return UriEncode(relative);
}

Path DeserializePath(StrView serialized, Status& status) {
  if (serialized.starts_with("file://")) return PathFromFileURI(serialized, status);
  Str relative = UriDecode(serialized);
#if defined(_WIN32)
  std::replace(relative.begin(), relative.end(), '/', '\\');
#endif
  return AutomatDir() / relative;
}

}  // namespace automat
