#include "log_skia.h"

#include "format.h"

namespace automat {

const Logger &operator<<(const Logger &logger, SkMatrix &m) {
  std::string out = "";
  for (int i = 0; i < 3; ++i) {
    out += "\n";
    for (int j = 0; j < 3; ++j) {
      int index = i * 3 + j;
      out += f("%.4f", m.get(index));
      if (j < 2) {
        out += ", ";
      }
    }
  }
  logger << out;
  return logger;
}

} // namespace automat