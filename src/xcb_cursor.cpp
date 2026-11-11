// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "xcb_cursor.hpp"

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <modules/svg/include/SkSVGDOM.h>
#include <xcb/render.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "format.hpp"
#include "int.hpp"
#include "libc.hpp"
#include "status.hpp"
#include "str.hpp"
#include "svg.hpp"
#include "vec.hpp"
#include "virtual_fs.hpp"
#include "xcb.hpp"

using namespace automat;

namespace xcb {

static constexpr U32 kXcursorImageType = 0xfffd0002;
static constexpr size_t kXcursorFileHeaderSize = 16;
static constexpr size_t kXcursorTableEntrySize = 12;
static constexpr size_t kXcursorImageHeaderSize = 36;
static constexpr U32 kMaxBundledCursorSize = 256;
static constexpr int kBundledCursorGrid = 24;

namespace {

struct CursorImage {
  U32 width;
  U32 height;
  U32 xhot;
  U32 yhot;
  const char* pixels;
};

struct BundledCursor {
  StrView name;
  int xhot;
  int yhot;
};

}  // namespace

static constexpr BundledCursor kBundledCursors[] = {
    {"left_ptr", 1, 1},    {"hand1", 8, 4},       {"xterm", 12, 12},     {"all-scroll", 12, 12},
    {"ew-resize", 12, 12}, {"ns-resize", 12, 12}, {"crosshair", 11, 11},
};

static StrView NextLine(StrView& text) {
  size_t end = text.find('\n');
  StrView line = text.substr(0, end);
  text.remove_prefix(end == StrView::npos ? text.size() : end + 1);
  return line;
}

static StrView ResourceValue(StrView resources, StrView name) {
  StrView value;
  while (!resources.empty()) {
    StrView line = NextLine(resources);
    if (!line.starts_with(name) || line.substr(name.size(), 1) != ":") continue;
    value = line.substr(name.size() + 1);
    value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
  }
  return value;
}

static U32 CursorSize(StrView resources) {
  if (const char* size = getenv("XCURSOR_SIZE"); size && atoi(size) > 0) {
    return atoi(size);
  }
  if (int size = atoi(Str(ResourceValue(resources, "Xcursor.size")).c_str()); size > 0) {
    return size;
  }
  if (int size = atoi(Str(ResourceValue(resources, "Xft.dpi")).c_str()) * 16 / 72; size > 0) {
    return size;
  }
  return std::min(screen->width_in_pixels, screen->height_in_pixels) / 48;
}

static Vec<Str> CursorDirectories() {
  const char* path = getenv("XCURSOR_PATH");
  StrView list =
      path ? path : "~/.icons:/usr/share/icons:/usr/share/pixmaps:/usr/X11R6/lib/X11/icons";
  const char* home = getenv("HOME");
  Vec<Str> directories;
  while (true) {
    size_t colon = list.find(':');
    StrView directory = list.substr(0, colon);
    if (directory.starts_with('~')) {
      directories.push_back(Str(home ? home : "") + Str(directory.substr(1)));
    } else {
      directories.push_back(Str(directory));
    }
    if (colon == StrView::npos) break;
    list.remove_prefix(colon + 1);
  }
  return directories;
}

static Vec<Str> InheritedThemes(StrView index_theme) {
  constexpr StrView kSeparators = " \t,;";
  Vec<Str> themes;
  while (!index_theme.empty()) {
    StrView line = NextLine(index_theme);
    if (!line.starts_with("Inherits")) continue;
    line.remove_prefix(8);
    line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
    if (!line.starts_with('=')) continue;
    line.remove_prefix(1);
    while (true) {
      size_t start = line.find_first_not_of(kSeparators);
      if (start == StrView::npos) break;
      line.remove_prefix(start);
      size_t end = std::min(line.find_first_of(kSeparators), line.size());
      themes.push_back(Str(line.substr(0, end)));
      line.remove_prefix(end);
    }
    break;
  }
  return themes;
}

static Str ReadThemeCursorFile(const Vec<Str>& directories, StrView theme, const char* name,
                               Vec<Str>& searched_themes) {
  if (std::find(searched_themes.begin(), searched_themes.end(), theme) != searched_themes.end()) {
    return {};
  }
  searched_themes.push_back(Str(theme));
  Vec<Str> inherited_themes;
  for (const Str& directory : directories) {
    Str theme_directory = f("{}/{}", directory, theme);
    Status status;
    Str file = fs::real.Read(Path(f("{}/cursors/{}", theme_directory, name)), status);
    if (OK(status)) return file;
    if (inherited_themes.empty()) {
      Status index_status;
      inherited_themes =
          InheritedThemes(fs::real.Read(Path(theme_directory + "/index.theme"), index_status));
    }
  }
  for (const Str& inherited_theme : inherited_themes) {
    Str file = ReadThemeCursorFile(directories, inherited_theme, name, searched_themes);
    if (!file.empty()) return file;
  }
  return {};
}

static U32 ReadU32(StrView file, size_t offset) {
  U32 value;
  memcpy(&value, file.data() + offset, sizeof(value));
  return value;
}

static std::optional<CursorImage> FindNearestImage(StrView file, U32 size) {
  if (file.size() < kXcursorFileHeaderSize || !file.starts_with("Xcur")) return std::nullopt;
  U32 table_position = ReadU32(file, 4);
  U32 entry_count = ReadU32(file, 12);
  if (table_position > file.size() ||
      entry_count > (file.size() - table_position) / kXcursorTableEntrySize) {
    return std::nullopt;
  }
  std::optional<size_t> nearest_entry;
  U32 nearest_distance = 0;
  for (U32 i = 0; i < entry_count; ++i) {
    size_t entry = table_position + i * kXcursorTableEntrySize;
    if (ReadU32(file, entry) != kXcursorImageType) continue;
    U32 nominal_size = ReadU32(file, entry + 4);
    U32 distance = nominal_size > size ? nominal_size - size : size - nominal_size;
    if (!nearest_entry || distance < nearest_distance) {
      nearest_entry = entry;
      nearest_distance = distance;
    }
  }
  if (!nearest_entry) return std::nullopt;
  U32 position = ReadU32(file, *nearest_entry + 8);
  if (position > file.size() || file.size() - position < kXcursorImageHeaderSize) {
    return std::nullopt;
  }
  if (ReadU32(file, position + 4) != kXcursorImageType ||
      ReadU32(file, position + 8) != ReadU32(file, *nearest_entry + 4)) {
    return std::nullopt;
  }
  CursorImage image = {
      .width = ReadU32(file, position + 16),
      .height = ReadU32(file, position + 20),
      .xhot = ReadU32(file, position + 24),
      .yhot = ReadU32(file, position + 28),
      .pixels = file.data() + position + kXcursorImageHeaderSize,
  };
  if (image.width == 0 || image.height == 0 || image.width > 0x7fff || image.height > 0x7fff ||
      U64(image.width) * image.height * 4 > file.size() - position - kXcursorImageHeaderSize) {
    return std::nullopt;
  }
  return image;
}

static std::optional<CursorImage> RenderBundledCursor(StrView name, U32 size, SkBitmap& bitmap) {
  const BundledCursor* cursor = std::ranges::find(kBundledCursors, name, &BundledCursor::name);
  if (cursor == std::end(kBundledCursors)) return std::nullopt;
  Status status;
  Str svg = fs::embedded.Read(Path(f("assets/cursors/{}.svg", name)), status);
  if (!OK(status)) return std::nullopt;
  sk_sp<SkSVGDOM> dom = SVGFromAsset(svg);
  if (!dom) return std::nullopt;
  dom->setContainerSize({kBundledCursorGrid, kBundledCursorGrid});
  bitmap.allocPixels(SkImageInfo::Make(size, size, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  SkCanvas canvas(bitmap);
  canvas.scale(float(size) / kBundledCursorGrid, float(size) / kBundledCursorGrid);
  dom->render(&canvas);
  return CursorImage{
      .width = size,
      .height = size,
      .xhot = U32(std::lrint(cursor->xhot * size / double(kBundledCursorGrid))),
      .yhot = U32(std::lrint(cursor->yhot * size / double(kBundledCursorGrid))),
      .pixels = (const char*)bitmap.getPixels(),
  };
}

static xcb_render_pictformat_t FindArgb32Format() {
  const xcb_query_extension_reply_t* render = xcb_get_extension_data(connection, &xcb_render_id);
  if (!render || !render->present) return XCB_NONE;
  std::unique_ptr<xcb_render_query_pict_formats_reply_t, libc::DeleteWithFree> reply(
      xcb_render_query_pict_formats_reply(connection, xcb_render_query_pict_formats(connection),
                                          nullptr));
  if (!reply) return XCB_NONE;
  for (auto it = xcb_render_query_pict_formats_formats_iterator(reply.get()); it.rem;
       xcb_render_pictforminfo_next(&it)) {
    const xcb_render_pictforminfo_t& format = *it.data;
    const xcb_render_directformat_t& direct = format.direct;
    if (format.type == XCB_RENDER_PICT_TYPE_DIRECT && format.depth == 32 &&
        direct.alpha_shift == 24 && direct.red_shift == 16 && direct.green_shift == 8 &&
        direct.blue_shift == 0 && direct.alpha_mask == 0xff && direct.red_mask == 0xff &&
        direct.green_mask == 0xff && direct.blue_mask == 0xff) {
      return format.id;
    }
  }
  return XCB_NONE;
}

xcb_cursor_t LoadCursor(const char* name) {
  xcb_render_pictformat_t argb32_format = FindArgb32Format();
  if (argb32_format == XCB_NONE) return XCB_NONE;

  Str resources = GetPropertyString(screen->root, XCB_ATOM_RESOURCE_MANAGER);
  Vec<Str> directories = CursorDirectories();
  const char* theme_variable = getenv("XCURSOR_THEME");
  StrView theme = theme_variable ? theme_variable : ResourceValue(resources, "Xcursor.theme");
  Vec<Str> searched_themes;
  Str file;
  if (!theme.empty()) file = ReadThemeCursorFile(directories, theme, name, searched_themes);
  if (file.empty()) file = ReadThemeCursorFile(directories, "default", name, searched_themes);
  U32 size = CursorSize(resources);
  SkBitmap bitmap;
  std::optional<CursorImage> image;
  if (file.empty()) {
    image = RenderBundledCursor(name, std::clamp<U32>(size, 1, kMaxBundledCursorSize), bitmap);
  } else {
    image = FindNearestImage(file, size);
  }
  if (!image) return XCB_NONE;

  xcb_pixmap_t pixmap = xcb_generate_id(connection);
  xcb_create_pixmap(connection, 32, pixmap, screen->root, image->width, image->height);
  xcb_gcontext_t gc = xcb_generate_id(connection);
  xcb_create_gc(connection, gc, pixmap, 0, nullptr);
  xcb_put_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, image->width, image->height, 0,
                0, 0, 32, image->width * image->height * 4, (const uint8_t*)image->pixels);
  xcb_render_picture_t picture = xcb_generate_id(connection);
  xcb_render_create_picture(connection, picture, pixmap, argb32_format, 0, nullptr);
  xcb_cursor_t cursor = xcb_generate_id(connection);
  xcb_render_create_cursor(connection, cursor, picture, image->xhot, image->yhot);
  xcb_render_free_picture(connection, picture);
  xcb_free_gc(connection, gc);
  xcb_free_pixmap(connection, pixmap);
  return cursor;
}

}  // namespace xcb
