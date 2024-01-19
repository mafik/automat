#include "hex.hh"

namespace maf {

void HexToBytesUnchecked(StrView hex, char *bytes) {
  bool high = true;
  for (int i = 0; i < hex.size(); i++) {
    char c = hex[i];
    if (c >= '0' && c <= '9') {
      c -= '0';
    } else if (c >= 'a' && c <= 'f') {
      c -= 'a' - 10;
    } else if (c >= 'A' && c <= 'F') {
      c -= 'A' - 10;
    } else {
      // ignore
    }
    if (high) {
      *bytes = c << 4;
      high = false;
    } else {
      *bytes |= c;
      bytes++;
      high = true;
    }
  }
}

Str BytesToHex(Span<> bytes) {
  Str result;
  result.reserve(bytes.size() * 2);
  for (U8 byte : bytes) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 0xf];
  }
  return result;
}

Str HexDump(StrView bytes) {
  Size n_lines = (bytes.size() + 15) / 16;
  int offset_width = 0;
  for (int x = n_lines; x; x >>= 4) {
    offset_width++;
  }
  Str result;
  // Every byte takes 3 characters in the output: 2 hex digits and a space.
  // Every line has 12 decoration characters.
  result.reserve(n_lines * (offset_width + 12 + 16 * 3) + 1);
  for (int line = 0; line < n_lines; ++line) {
    // Print the starting offset of the current line
    for (int off_char = offset_width - 1; off_char >= 0; --off_char) {
      result += "0123456789abcdef"[(line >> (off_char * 4)) & 0xf];
    }
    result += "0: ";
    // Print the hex values of the current line
    for (int col = 0; col < 16; ++col) {
      int byte = line * 16 + col;
      if (byte < bytes.size()) {
        result += "0123456789abcdef"[bytes[byte] >> 4];
        result += "0123456789abcdef"[bytes[byte] & 0xf];
      } else {
        result += "  ";
      }
      if (col % 4 == 3) {
        result += " ";
      }
      if (col % 8 == 7) { // extra space after 8 bytes
        result += " ";
      }
    }
    // Print the ASCII values of the current line
    result += "|";
    for (int col = 0; col < 16; ++col) {
      int byte = line * 16 + col;
      if (byte < bytes.size()) {
        char c = bytes[byte];
        if (c < 32 || c > 126) {
          c = '.';
        }
        result += c;
      } else {
        result += " ";
      }
    }
    result += "|";
    result += "\n";
  }
  return result;
}

Str HexDump(Span<> bytes) { return HexDump(StrViewOf(bytes)); }

} // namespace maf