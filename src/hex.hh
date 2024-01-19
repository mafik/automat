#pragma once

#include "arr.hh"
#include "span.hh"
#include "str.hh"
#include "vec.hh"

namespace maf {

void HexToBytesUnchecked(StrView hex, char* out_bytes);

Str BytesToHex(Span<> bytes);

inline Str BytesToHex(const char* bytes, size_t len) {
  return BytesToHex(Span<>{const_cast<char*>(bytes), len});
}

constexpr U8 HexToU8(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

constexpr U8 HexToU8(const char c[2]) { return (HexToU8(c[0]) << 4) | HexToU8(c[1]); }

template <typename T>
inline Str ValToHex(const T& val) {
  return BytesToHex(Span<>((char*)(&val), sizeof(T)));
}

inline Vec<> operator""_HexVec(const char* str, size_t len) {
  Vec buf;
  buf.resize(len / 2);
  HexToBytesUnchecked({str, len}, buf.data());
  return buf;
}

template <size_t N>
constexpr Arr<char, (N - 1) / 2> HexArr(const char (&str)[N]) {
  Arr<char, (N - 1) / 2> arr;
  HexToBytesUnchecked(StrView(str, N - 1), arr.data());
  return arr;
}

// Print the given bytes as a hex dump.
//
// Each printed live covers 16 bytes.
// Left side has hex offsets, then 16-column hex string with spaces between
// every 4 bytes and on the right side - ASCII (or '.').
Str HexDump(StrView bytes);
Str HexDump(Span<> bytes);

}  // namespace maf