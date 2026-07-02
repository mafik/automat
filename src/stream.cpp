// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "stream.hpp"

#include "format.hpp"

namespace automat {

Str FormatBytes(uint64_t bytes) {
  if (bytes < 1024) return f("{} B", bytes);
  if (bytes < 1024 * 1024) {
    if (bytes % 1024 == 0) return f("{} KiB", bytes / 1024);
    return f("{:.1f} KiB", bytes / 1024.0);
  }
  return f("{:.1f} MiB", bytes / (1024.0 * 1024.0));
}

Str FormatBytesPerSecond(double bps) {
  if (bps < 1) return "0 B/s";
  if (bps < 1024) return f("{} B/s", (int)bps);
  if (bps < 1024 * 1024) return f("{:.1f} kB/s", bps / 1024);
  if (bps < 1024 * 1024 * 1024) return f("{:.1f} MB/s", bps / (1024 * 1024));
  return f("{:.1f} GB/s", bps / (1024 * 1024 * 1024));
}

}  // namespace automat
