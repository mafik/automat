// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include <include/core/SkBitmap.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/encode/SkWebpEncoder.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#pragma maf main
#pragma comment(lib, "skia")

// Tile structure
struct Tile {
  int x, y;
  std::unique_ptr<uint8_t[]> bgra_planes;  // 64*64*4 bytes in planar BGRA format
};

// LZF decompressor
std::unique_ptr<uint8_t[]> lzf_decompress(const uint8_t* data, size_t size, size_t expected_size) {
  auto output = std::make_unique<uint8_t[]>(expected_size);
  size_t out_pos = 0;
  size_t in_pos = 0;

  while (in_pos < size && out_pos < expected_size) {
    uint8_t ctrl = data[in_pos++];

    if (ctrl < 32) {  // Literal run
      size_t length = ctrl + 1;
      if (in_pos + length > size) break;
      memcpy(output.get() + out_pos, data + in_pos, length);
      out_pos += length;
      in_pos += length;
    } else {  // Back reference
      size_t length = ctrl >> 5;
      if (length == 7) {
        if (in_pos >= size) break;
        length += data[in_pos++];
      }
      length += 2;

      if (in_pos >= size) break;
      size_t offset = ((ctrl & 0x1f) << 8) | data[in_pos++];
      offset += 1;

      size_t start_pos = out_pos - offset;
      if (start_pos > out_pos) break;  // Invalid offset

      for (size_t j = 0; j < length && out_pos < expected_size; j++) {
        output[out_pos++] = output[start_pos + j];
      }
    }
  }

  return output;
}

// Read entire stdin into a buffer
std::vector<uint8_t> read_stdin() {
  std::vector<uint8_t> buffer;
  uint8_t temp[4096];

  while (true) {
    size_t n = fread(temp, 1, sizeof(temp), stdin);
    if (n == 0) break;
    buffer.insert(buffer.end(), temp, temp + n);
  }

  return buffer;
}

// Parse a line from buffer
bool parse_line(const uint8_t* data, size_t size, size_t& pos, char* line, size_t max_len) {
  size_t start = pos;
  while (pos < size && data[pos] != '\n') {
    pos++;
  }

  size_t len = pos - start;
  if (len >= max_len) return false;

  memcpy(line, data + start, len);
  line[len] = '\0';

  if (pos < size) pos++;  // Skip newline
  return true;
}

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif

  // Read entire input
  auto input_data = read_stdin();
  if (input_data.empty()) {
    fprintf(stderr, "Error: No input data\n");
    return 1;
  }

  // Parse header
  size_t pos = 0;
  char line[256];

  if (!parse_line(input_data.data(), input_data.size(), pos, line, sizeof(line))) {
    fprintf(stderr, "Error: Failed to parse VERSION line\n");
    return 2;
  }

  int version;
  if (sscanf(line, "VERSION %d", &version) != 1 || version != 2) {
    fprintf(stderr, "Error: Invalid or unsupported version: %s\n", line);
    return 3;
  }

  if (!parse_line(input_data.data(), input_data.size(), pos, line, sizeof(line))) {
    fprintf(stderr, "Error: Failed to parse TILEWIDTH\n");
    return 4;
  }

  int tile_width;
  if (sscanf(line, "TILEWIDTH %d", &tile_width) != 1) {
    fprintf(stderr, "Error: Invalid TILEWIDTH: %s\n", line);
    return 5;
  }

  if (!parse_line(input_data.data(), input_data.size(), pos, line, sizeof(line))) {
    fprintf(stderr, "Error: Failed to parse TILEHEIGHT\n");
    return 6;
  }

  int tile_height;
  if (sscanf(line, "TILEHEIGHT %d", &tile_height) != 1) {
    fprintf(stderr, "Error: Invalid TILEHEIGHT: %s\n", line);
    return 7;
  }

  if (!parse_line(input_data.data(), input_data.size(), pos, line, sizeof(line))) {
    fprintf(stderr, "Error: Failed to parse PIXELSIZE\n");
    return 8;
  }

  int pixel_size;
  if (sscanf(line, "PIXELSIZE %d", &pixel_size) != 1 || pixel_size != 4) {
    fprintf(stderr, "Error: Invalid or unsupported PIXELSIZE: %s\n", line);
    return 9;
  }

  // Skip DATA line
  if (!parse_line(input_data.data(), input_data.size(), pos, line, sizeof(line))) {
    fprintf(stderr, "Error: Failed to parse DATA line\n");
    return 10;
  }

  // Parse tiles
  std::vector<Tile> tiles;

  while (pos < input_data.size()) {
    // Parse tile header
    if (!parse_line(input_data.data(), input_data.size(), pos, line, sizeof(line))) {
      break;
    }

    if (strlen(line) == 0 || strchr(line, ',') == nullptr) {
      break;
    }

    int tile_x, tile_y, compressed_size;
    char compression[16];
    if (sscanf(line, "%d,%d,%15[^,],%d", &tile_x, &tile_y, compression, &compressed_size) != 4) {
      break;
    }

    if (pos + compressed_size > input_data.size()) {
      fprintf(stderr, "Error: Truncated tile data\n");
      return 11;
    }

    const uint8_t* tile_data = input_data.data() + pos;
    pos += compressed_size;

    // Skip compression flag byte and decompress
    if (compressed_size < 1) continue;

    uint8_t compression_flag = tile_data[0];
    const uint8_t* payload = tile_data + 1;
    size_t payload_size = compressed_size - 1;

    size_t expected_size = tile_width * tile_height * pixel_size;
    std::unique_ptr<uint8_t[]> decompressed;

    if (strcmp(compression, "LZF") == 0 && compression_flag == 1) {
      decompressed = lzf_decompress(payload, payload_size, expected_size);
    } else {
      // Uncompressed
      decompressed = std::make_unique<uint8_t[]>(expected_size);
      memcpy(decompressed.get(), payload, std::min(payload_size, expected_size));
    }

    tiles.push_back(Tile{tile_x, tile_y, std::move(decompressed)});
  }

  if (tiles.empty()) {
    fprintf(stderr, "Error: No tiles found\n");
    return 12;
  }

  // Sort tiles
  std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) {
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
  });

  // Find bounds using alpha channel
  int x_min = INT32_MAX, x_max = INT32_MIN;
  int y_min = INT32_MAX, y_max = INT32_MIN;

  int tile_pixel_count = tile_width * tile_height;

  for (const auto& tile : tiles) {
    const uint8_t* alpha_plane = tile.bgra_planes.get() + tile_pixel_count * 3;

    for (int ty = 0; ty < tile_height; ty++) {
      for (int tx = 0; tx < tile_width; tx++) {
        int pixel_idx = ty * tile_width + tx;
        uint8_t alpha = alpha_plane[pixel_idx];

        if (alpha > 0) {
          int px = tile.x + tx;
          int py = tile.y + ty;

          x_min = std::min(x_min, px);
          x_max = std::max(x_max, px);
          y_min = std::min(y_min, py);
          y_max = std::max(y_max, py);
        }
      }
    }
  }

  if (x_min > x_max || y_min > y_max) {
    fprintf(stderr, "Error: Layer is completely transparent\n");
    return 13;
  }

  int width = x_max - x_min + 1;
  int height = y_max - y_min + 1;

  fprintf(stderr, "WIDTH %d\n", width);
  fprintf(stderr, "HEIGHT %d\n", height);
  fprintf(stderr, "TRIMMED_X %d\n", x_min);
  fprintf(stderr, "TRIMMED_Y %d\n", y_min);

  // Create output bitmap
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(
          SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType))) {
    fprintf(stderr, "Error: Failed to allocate bitmap\n");
    return 14;
  }

  // Clear to transparent
  bitmap.eraseColor(0);

  SkPixmap pixmap;
  if (!bitmap.peekPixels(&pixmap)) {
    fprintf(stderr, "Error: Failed to get pixmap\n");
    return 15;
  }

  // Copy tiles into bitmap
  for (const auto& tile : tiles) {
    const uint8_t* b_plane = tile.bgra_planes.get();
    const uint8_t* g_plane = b_plane + tile_pixel_count;
    const uint8_t* r_plane = g_plane + tile_pixel_count;
    const uint8_t* a_plane = r_plane + tile_pixel_count;

    for (int ty = 0; ty < tile_height; ty++) {
      for (int tx = 0; tx < tile_width; tx++) {
        int px = tile.x + tx;
        int py = tile.y + ty;

        // Check if pixel is within trimmed bounds
        if (px < x_min || px > x_max || py < y_min || py > y_max) {
          continue;
        }

        int pixel_idx = ty * tile_width + tx;
        uint8_t b = b_plane[pixel_idx];
        uint8_t g = g_plane[pixel_idx];
        uint8_t r = r_plane[pixel_idx];
        uint8_t a = a_plane[pixel_idx];

        // Write to bitmap (RGBA format)
        int bx = px - x_min;
        int by = py - y_min;

        uint8_t* pixel = (uint8_t*)pixmap.writable_addr() + (by * pixmap.rowBytes()) + (bx * 4);
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = a;
      }
    }
  }

  // Encode as WebP
  SkDynamicMemoryWStream stream;
  SkWebpEncoder::Options options;
  options.fQuality = 95;

  if (!SkWebpEncoder::Encode(&stream, pixmap, options)) {
    fprintf(stderr, "Error: Failed to encode WebP\n");
    return 16;
  }

  // Write to stdout
  auto data = stream.detachAsData();
  if (fwrite(data->data(), 1, data->size(), stdout) != data->size()) {
    fprintf(stderr, "Error: Failed to write WebP to stdout\n");
    return 17;
  }

  stream.flush();

  return 0;
}
