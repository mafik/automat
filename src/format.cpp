// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "format.hpp"

#include <fmt/format.h>
#include <src/base/SkUTF.h>

#include <cstring>
#include <ranges>

#include "hex.hpp"
#include "int.hpp"

#pragma comment(lib, "skia")

namespace automat {

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

static bool IsEscapedControl(SkUnichar c) { return c == '\t' || c == '\n' || c == '\r'; }

static int Columns(SkUnichar c) { return IsEscapedControl(c) ? 2 : 1; }

Str BlobSummary(StrView blob, int max_columns) {
  if (blob.empty()) return "(0 B)";
  bool is_utf8 = true;
  {  // UTF-8 test
    const char* begin = blob.data();
    const char* end = (blob.data() + blob.size());
    int n_ascii = 0, n_non_ascii = 0;
    while (begin < end) {
      auto c = SkUTF::NextUTF8(&begin, end);
      if (c < 32 && !IsEscapedControl(c) || (c >= 0x7F && c <= 0x9F)) {
        is_utf8 = false;
        break;
      }
      if (c == 0xFEFF) continue;
      ++(c < 128 ? n_ascii : n_non_ascii);
    }
    if (n_ascii < n_non_ascii) {
      is_utf8 = false;
    }
  }
  bool is_utf16 = true;
  {  // UTF-16 test
    const uint16_t* begin = (const uint16_t*)blob.data();
    const uint16_t* end = (const uint16_t*)(blob.data() + blob.size());
    int n_ascii = 0, n_non_ascii = 0;
    while (begin < end) {
      auto c = SkUTF::NextUTF16(&begin, end);
      if (c < 32 && !IsEscapedControl(c) || (c >= 0x7F && c <= 0x9F)) {
        is_utf16 = false;
        break;
      }
      if (c == 0xFEFF) continue;
      ++(c < 128 ? n_ascii : n_non_ascii);
    }
    if (n_ascii < n_non_ascii) {
      is_utf16 = false;
    }
  }
  SmallVec<SkUnichar, 64> prefix, suffix;
  Str out;
  if (is_utf8) {
    out = f("(utf-8 {} B)", blob.size());
    max_columns -= out.size();
    if (max_columns > 0) {
      max_columns -= 1;
      out += ' ';
    }
    while (max_columns > 0 && !blob.empty()) {
      if (prefix.size() <= suffix.size()) {
        const char* p = blob.data();
        auto c = SkUTF::NextUTF8(&p, (blob.data() + blob.size()));
        if (Columns(c) > max_columns) break;
        blob.remove_prefix(p - blob.data());
        if (c == '\t') {
          prefix.push_back('\\');
          prefix.push_back('t');
          max_columns -= 2;
        } else if (c == '\n') {
          prefix.push_back('\\');
          prefix.push_back('n');
          max_columns -= 2;
        } else if (c == '\r') {
          prefix.push_back('\\');
          prefix.push_back('r');
          max_columns -= 2;
        } else {
          prefix.push_back(c);
          max_columns -= 1;
        }
      } else {
        const char* p = (blob.data() + blob.size()) - 1;
        while (((U8)*p & 0xC0) == 0x80) --p;
        const char* q = p;
        auto c = SkUTF::NextUTF8(&q, (blob.data() + blob.size()));
        if (Columns(c) > max_columns) break;
        blob.remove_suffix((blob.data() + blob.size()) - p);
        if (c == '\t') {
          suffix.push_back('t');
          suffix.push_back('\\');
          max_columns -= 2;
        } else if (c == '\n') {
          suffix.push_back('n');
          suffix.push_back('\\');
          max_columns -= 2;
        } else if (c == '\r') {
          suffix.push_back('r');
          suffix.push_back('\\');
          max_columns -= 2;
        } else {
          suffix.push_back(c);
          max_columns -= 1;
        }
      }
    }
  } else if (is_utf16) {
    out = f("(utf-16 {} B)", blob.size());
    max_columns -= out.size();
    if (max_columns > 0) {
      max_columns -= 1;
      out += ' ';
    }
    while (max_columns > 0 && !blob.empty()) {
      if (prefix.size() <= suffix.size()) {
        const uint16_t* p = (const uint16_t*)blob.data();
        auto c = SkUTF::NextUTF16(&p, (const uint16_t*)(blob.data() + blob.size()));
        if (Columns(c) > max_columns) break;
        blob.remove_prefix((intptr_t)p - (intptr_t)blob.data());
        if (c == '\t') {
          prefix.push_back('\\');
          prefix.push_back('t');
          max_columns -= 2;
        } else if (c == '\n') {
          prefix.push_back('\\');
          prefix.push_back('n');
          max_columns -= 2;
        } else if (c == '\r') {
          prefix.push_back('\\');
          prefix.push_back('r');
          max_columns -= 2;
        } else {
          prefix.push_back(c);
          max_columns -= 1;
        }
      } else {
        const uint16_t* p = ((const uint16_t*)(blob.data() + blob.size())) - 1;
        if (SkUTF::IsTrailingSurrogateUTF16(*p)) --p;
        const uint16_t* q = p;
        auto c = SkUTF::NextUTF16(&q, (const uint16_t*)(blob.data() + blob.size()));
        if (Columns(c) > max_columns) break;
        blob.remove_suffix((intptr_t)(blob.data() + blob.size()) - (intptr_t)p);
        if (c == '\t') {
          suffix.push_back('t');
          suffix.push_back('\\');
          max_columns -= 2;
        } else if (c == '\n') {
          suffix.push_back('n');
          suffix.push_back('\\');
          max_columns -= 2;
        } else if (c == '\r') {
          suffix.push_back('r');
          suffix.push_back('\\');
          max_columns -= 2;
        } else {
          suffix.push_back(c);
          max_columns -= 1;
        }
      }
    }
  } else {  // Hex dump: every byte prints ASCII-or-dot + two-digit hex
    out = f("(blob {} B)", blob.size());
    max_columns -= out.size();
    if (max_columns > 0) {
      max_columns -= 1;
      out += ' ';
    }
    max_columns -= 2;  // two spaces between ASCII & hex
    if (max_columns <= 0) {
      // that's enough!
    } else if (blob.size() > max_columns / 3) {  // ellipsized hex dump
      max_columns -= 2;                          // use two columns for '|'
      int n_chars = max_columns > 0 ? max_columns / 3 : 0;
      int prefix_bytes = (n_chars + 1) / 2;
      int suffix_bytes = n_chars / 2;
      for (int i = 0; i < prefix_bytes; ++i) {  // ASCII 1st half
        prefix.push_back(PrintableOrDot(blob[i]));
      }
      prefix.push_back('|');
      for (int i = blob.size() - suffix_bytes; i < blob.size(); ++i) {  // ASCII 2nd half
        prefix.push_back(PrintableOrDot(blob[i]));
      }
      prefix.push_back(' ');
      prefix.push_back(' ');
      for (int i = 0; i < prefix_bytes; ++i) {  // Hex 1st half
        auto hex = ByteToHex(blob[i]);
        prefix.push_back(hex.first);
        prefix.push_back(hex.second);
      }
      prefix.push_back('|');
      for (int i = blob.size() - suffix_bytes; i < blob.size(); ++i) {  // Hex 2nd half
        auto hex = ByteToHex(blob[i]);
        prefix.push_back(hex.first);
        prefix.push_back(hex.second);
      }
    } else {  // continuous hex dump
      for (int i = 0; i < blob.size(); ++i) {
        prefix.push_back(PrintableOrDot(blob[i]));
      }
      prefix.push_back(' ');
      prefix.push_back(' ');
      for (int i = 0; i < blob.size(); ++i) {
        auto hex = ByteToHex(blob[i]);
        prefix.push_back(hex.first);
        prefix.push_back(hex.second);
      }
    }
  }
  for (auto c : prefix) {
    char buf[8];
    int n = SkUTF::ToUTF8(c, buf);
    out.append(buf, n);
  }
  if (!suffix.empty()) {
    if (!blob.empty()) {
      out.append("...");
      suffix.pop_back_n(3);  // to make space for ellipsis
    }
    for (auto c : std::ranges::reverse_view(suffix)) {
      char buf[8];
      int n = SkUTF::ToUTF8(c, buf);
      out.append(buf, n);
    }
  }
  return out;
}

std::string_view CleanTypeName(std::string_view mangled) {
#ifdef _WIN32
  // On Windows we get a long name that starts with a struct and then a sequence of namespaces:
  // "struct automat::library::FlipFlopButton"
  // We extract just the last component.
  if (mangled.starts_with("struct ")) {
    mangled.remove_prefix(7);
  }
  for (int i = mangled.size() - 2; i > 0; --i) {
    if (mangled[i] == ':' && mangled[i + 1] == ':') {
      mangled.remove_prefix(i + 2);
      break;
    }
  }
  return mangled;
#else
  // On Linux we get a C++-mangled name:
  // "N7automat7library14FlipFlopButtonE"
  if (mangled.starts_with("N") && mangled.ends_with("E")) {
    mangled.remove_prefix(1);  // Remove 'N'
    mangled.remove_suffix(1);  // Remove 'E'
    while (mangled.size() > 1 && mangled[0] >= '0' && mangled[0] <= '9') {
      // Parse the length of the next component
      size_t length = 0;
      size_t i = 0;
      while (i < mangled.size() && mangled[i] >= '0' && mangled[i] <= '9') {
        length = length * 10 + (mangled[i] - '0');
        i++;
      }
      // Skip this component
      if (i + length < mangled.size()) {
        mangled.remove_prefix(i + length);
      } else {
        mangled.remove_prefix(i);  // final component - remove only its length
        break;
      }
    }
  }
  return mangled;
#endif
}
}  // namespace automat