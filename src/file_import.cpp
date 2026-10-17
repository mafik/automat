// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "file_import.hpp"

#include <sys/stat.h>

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
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "format.hpp"
#include "virtual_fs.hpp"

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

bool SameFilesystem(const Path& a, const Path& b) {
  struct stat sa, sb;
  if (stat(a.c_str(), &sa) != 0) return false;
  if (stat(b.c_str(), &sb) != 0) return false;
  return sa.st_dev == sb.st_dev;
}

uint64_t FileSize(const Path& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

void CopyInto(const Path& src, const Path& dst, Status& status, std::atomic<uint64_t>* progress,
              const std::atomic<bool>* cancelled) {
#if defined(_WIN32)
  struct Report {
    std::atomic<uint64_t>* progress;
    const std::atomic<bool>* cancelled;
  } report = {progress, cancelled};
  auto callback = [](LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD,
                     DWORD, HANDLE, HANDLE, LPVOID data) -> DWORD {
    auto& report = *static_cast<Report*>(data);
    if (report.progress) report.progress->store(transferred.QuadPart, std::memory_order_relaxed);
    if (report.cancelled && report.cancelled->load(std::memory_order_relaxed)) {
      return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
  };
  if (!CopyFileExA(src.c_str(), dst.c_str(), callback, &report, nullptr, 0) &&
      GetLastError() != ERROR_REQUEST_ABORTED) {
    AppendErrorMessage(status) +=
        f("CopyFileEx({}, {}): error {}", src.str, dst.str, GetLastError());
  }
#else
  uint64_t total = FileSize(src);
  int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    AppendErrorMessage(status) += f("open({}): {}", src.str, strerror(errno));
    return;
  }
  int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    AppendErrorMessage(status) += f("open({}): {}", dst.str, strerror(errno));
    close(in);
    return;
  }
#if defined(__linux__)
  if (ioctl(out, FICLONE, in) == 0) {
    if (progress) progress->store(total, std::memory_order_relaxed);
    close(in);
    close(out);
    return;
  }
#endif
  char buf[1 << 16];
  uint64_t done = 0;
  ssize_t n = 0;
  while (!(cancelled && cancelled->load(std::memory_order_relaxed)) &&
         (n = read(in, buf, sizeof(buf))) > 0) {
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = write(out, buf + written, n - written);
      if (w < 0) {
        AppendErrorMessage(status) += f("write({}): {}", dst.str, strerror(errno));
        close(in);
        close(out);
        return;
      }
      written += w;
    }
    done += (uint64_t)n;
    if (progress) progress->store(done, std::memory_order_relaxed);
  }
  if (n < 0) {
    AppendErrorMessage(status) += f("read({}): {}", src.str, strerror(errno));
  }
  close(in);
  close(out);
#endif
}

void WriteInto(const Path& dst, StrView bytes, Status& status) {
  fs::real.Write(dst, bytes, status);
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

}  // namespace automat
