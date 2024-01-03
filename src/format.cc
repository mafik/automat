#include "format.hh"

#include <cstdarg>
#include <cstdio>

namespace maf {

std::string f(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list args2;
  va_copy(args2, args);
  int n = vsnprintf(NULL, 0, fmt, args) + 1;
  char buf[n];
  vsnprintf(buf, sizeof(buf), fmt, args2);
  va_end(args);
  return std::string(buf);
}

std::string IndentString(std::string in, int spaces) {
  std::string out(spaces, ' ');
  for (char c : in) {
    out += c;
    if (c == '\n') {
      for (int i = 0; i < spaces; ++i) {
        out += ' ';
      }
    }
  }
  return out;
}

std::string Slugify(std::string in) {
  std::string out;
  bool unk = false;
  for (char c : in) {
    if (c >= 'A' && c <= 'Z') {
      if (unk) {
        if (!out.empty()) {
          out += '-';
        }
        unk = false;
      }
      out += c - 'A' + 'a';
    } else if (c >= 'a' && c <= 'z') {
      if (unk) {
        if (!out.empty()) {
          out += '-';
        }
        unk = false;
      }
      out += c;
    } else if (c >= '0' && c <= '9') {
      if (unk) {
        if (!out.empty()) {
          out += '-';
        }
        unk = false;
      }
      out += c;
    } else {
      unk = true;
    }
  }
  return out;
}

} // namespace maf