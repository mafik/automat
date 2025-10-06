// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "log_skia.hh"

#include "format.hh"
#include "log.hh"

namespace automat {

const LogEntry& operator<<(const LogEntry& logger, SkMatrix& m) {
  int prefix_length = logger.buffer.size();
  std::string out = f("[{:.4f}, {:.4f}, {:.4f}]\n", m.get(0), m.get(1), m.get(2));
  for (int i = 0; i < prefix_length; ++i) out += ' ';
  out += f("[{:.4f}, {:.4f}, {:.4f}]\n", m.get(3), m.get(4), m.get(5));
  for (int i = 0; i < prefix_length; ++i) out += ' ';
  out += f("[{:.4f}, {:.4f}, {:.4f}]", m.get(6), m.get(7), m.get(8));
  logger << out;
  return logger;
}

}  // namespace automat