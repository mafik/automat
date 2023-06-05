#include "format.h"

#include <stdarg.h>

#include <cstdio>

std::string f(const char* fmt, ...) {
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
