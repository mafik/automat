// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

#include "format.hh"

// See: https://en.wikipedia.org/wiki/ANSI_escape_code

namespace term {

#define TERM_CODE(C) "\033[" #C "m"
#define TERM_CODE_BOLD TERM_CODE(1)
#define TERM_CODE_DIM TERM_CODE(2)
#define TERM_CODE_ITALIC TERM_CODE(3)
#define TERM_CODE_UNDERLINE TERM_CODE(4)
#define TERM_CODE_BLINK TERM_CODE(5)
#define TERM_CODE_FAST_BLINK TERM_CODE(6)
#define TERM_CODE_INVERT TERM_CODE(7)
#define TERM_CODE_HIDDEN TERM_CODE(8)
#define TERM_CODE_STRIKE TERM_CODE(9)

#define TERM_CODE_FONT_DEFAULT \
  TERM_CODE(10)  // codes 11-19 are for alternative fonts but they're poorly
                 // supported
#define TERM_CODE_FONT_BLACKLETTER TERM_CODE(20)  // also poorly supported

#define TERM_CODE_RESET_ALL TERM_CODE(0)
#define TERM_CODE_RESET_FONT TERM_CODE(20)
#define TERM_CODE_RESET_FG TERM_CODE(39)
#define TERM_CODE_RESET_BG TERM_CODE(49)

#define TERM_CODE_DOUBLE_UNDERLINE \
  TERM_CODE(21)  // Note that these two are the same! Different terminals messed
                 // things up
#define TERM_CODE_RESET_BOLD TERM_CODE(21)
#define TERM_CODE_RESET_DIM_BOLD TERM_CODE(22)
#define TERM_CODE_RESET_ITALIC TERM_CODE(23)
#define TERM_CODE_RESET_UNDERLINE TERM_CODE(24)
#define TERM_CODE_RESET_BLINK TERM_CODE(25)
#define TERM_CODE_RESET_INVERT TERM_CODE(27)
#define TERM_CODE_RESET_HIDDEN TERM_CODE(28)
#define TERM_CODE_RESET_STRIKE TERM_CODE(29)

#define TERM_CODE_FG_BLACK TERM_CODE(30)
#define TERM_CODE_FG_DARK_RED TERM_CODE(31)
#define TERM_CODE_FG_DARK_GREEN TERM_CODE(32)
#define TERM_CODE_FG_DARK_YELLOW TERM_CODE(33)
#define TERM_CODE_FG_DARK_BLUE TERM_CODE(34)
#define TERM_CODE_FG_DARK_MAGENTA TERM_CODE(35)
#define TERM_CODE_FG_DARK_CYAN TERM_CODE(36)
#define TERM_CODE_FG_LIGHT_GRAY TERM_CODE(37)
#define TERM_CODE_FG_DARK_GRAY TERM_CODE(90)
#define TERM_CODE_FG_LIGHT_RED TERM_CODE(91)
#define TERM_CODE_FG_LIGHT_GREEN TERM_CODE(92)
#define TERM_CODE_FG_LIGHT_YELLOW TERM_CODE(93)
#define TERM_CODE_FG_LIGHT_BLUE TERM_CODE(94)
#define TERM_CODE_FG_LIGHT_MAGENTA TERM_CODE(95)
#define TERM_CODE_FG_LIGHT_CYAN TERM_CODE(96)
#define TERM_CODE_FG_WHITE TERM_CODE(97)

#define TERM_CODE_FG_256(N) "\033[38;5;" #N "m"
#define TERM_CODE_BG_256(N) "\033[48;5;" #N "m"

#define TERM_CODE_FG_RGB(R, G, B) "\033[38;2;" #R ";" #G ";" #B "m"
#define TERM_CODE_BG_RGB(R, G, B) "\033[48;2;" #R ";" #G ";" #B "m"

int GrayIndex256(int shade) {
  if (shade == 0) return 0;
  if (shade >= 25) return 255;
  return shade += 231;
}

int ColorIndex256(int r, int g, int b) {
  r = std::clamp(r, 0, 5);
  g = std::clamp(g, 0, 5);
  b = std::clamp(b, 0, 5);
  return 16 + b + g * 6 + r * 36;
}

std::string CodeFg256(int index) { return f("\033[38;5;{}m", index); }

struct Fg256 {
  int index;
  Fg256(int index_arg) : index(index_arg) {}
  std::string operator()(std::string val) { return CodeFg256(index) + val + TERM_CODE_RESET_FG; }
};

Fg256 Gray(int shade) { return Fg256(GrayIndex256(shade)); }

std::string Italic(std::string arg) {
  return std::string(TERM_CODE_ITALIC) + arg + TERM_CODE_RESET_ITALIC;
}

std::string Red(std::string arg) {
  return std::string(TERM_CODE_FG_LIGHT_RED) + arg + TERM_CODE_RESET_FG;
}

}  // namespace term
