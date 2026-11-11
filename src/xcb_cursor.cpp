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
#include <span>

#include "crt.hpp"
#include "format.hpp"
#include "int.hpp"
#include "pointer.hpp"
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
  U32 delay;
  const char* pixels;
};

struct CursorDescription {
  ui::Cursor cursor;
  StrView name;
  int xhot;
  int yhot;
  std::span<const U32> frame_delays;
};

}  // namespace

static const CursorDescription* FindCursorDescription(ui::Cursor icon) {
  using enum ui::Cursor;
  static constexpr U32 kProgressAndWaitDelays[] = {30, 30, 30, 30, 300, 30, 30, 30,
                                                   30, 30, 30, 30, 30,  30, 30, 30};
  static constexpr CursorDescription kCursors[] = {
      {Default, "default", 1, 1},
      {ContextMenu, "context-menu", 1, 1},
      {Help, "help", 1, 1},
      {Pointer, "pointer", 8, 4},
      {Progress, "progress", 1, 1, kProgressAndWaitDelays},
      {Wait, "wait", 12, 12, kProgressAndWaitDelays},
      {Cell, "cell", 11, 11},
      {Crosshair, "crosshair", 11, 11},
      {Text, "text", 12, 12},
      {VerticalText, "vertical-text", 12, 12},
      {Alias, "alias", 1, 1},
      {Copy, "copy", 1, 1},
      {Move, "move", 12, 12},
      {NoDrop, "no-drop", 1, 1},
      {NotAllowed, "not-allowed", 12, 12},
      {Grab, "grab", 12, 15},
      {Grabbing, "grabbing", 12, 15},
      {EResize, "e-resize", 20, 11},
      {NResize, "n-resize", 11, 3},
      {NeResize, "ne-resize", 20, 3},
      {NwResize, "nw-resize", 3, 3},
      {SResize, "s-resize", 11, 19},
      {SeResize, "se-resize", 20, 20},
      {SwResize, "sw-resize", 3, 20},
      {WResize, "w-resize", 3, 11},
      {EwResize, "ew-resize", 12, 12},
      {NsResize, "ns-resize", 12, 12},
      {NeswResize, "nesw-resize", 12, 12},
      {NwseResize, "nwse-resize", 12, 12},
      {ColResize, "col-resize", 12, 12},
      {RowResize, "row-resize", 12, 12},
      {AllScroll, "all-scroll", 12, 12},
      {ZoomIn, "zoom-in", 9, 9},
      {ZoomOut, "zoom-out", 9, 9},
      {DndAsk, "dnd-ask", 1, 1},
      {AllResize, "all-resize", 12, 12},
      {BasedArrowDown, "based_arrow_down", 11, 18},
      {BasedArrowUp, "based_arrow_up", 11, 3},
      {BottomTee, "bottom_tee", 11, 19},
      {CenterPtr, "center_ptr", 11, 1},
      {CoffeeMug, "coffee_mug", 6, 12},
      {ColorPicker, "color-picker", 1, 20},
      {DndMove, "dnd-move", 1, 1},
      {Draft, "draft", 3, 20},
      {DrapedBox, "draped_box", 12, 12},
      {Exchange, "exchange", 12, 12},
      {LeftTee, "left_tee", 3, 11},
      {LlAngle, "ll_angle", 3, 20},
      {LrAngle, "lr_angle", 20, 20},
      {Pencil, "pencil", 3, 19},
      {Pirate, "pirate", 12, 12},
      {RightPtr, "right_ptr", 22, 1},
      {RightTee, "right_tee", 20, 11},
      {SbDownArrow, "sb_down_arrow", 11, 20},
      {SbLeftArrow, "sb_left_arrow", 2, 11},
      {SbRightArrow, "sb_right_arrow", 21, 11},
      {SbUpArrow, "sb_up_arrow", 11, 1},
      {TopTee, "top_tee", 11, 2},
      {UlAngle, "ul_angle", 3, 3},
      {UrAngle, "ur_angle", 20, 3},
      {WaylandCursor, "wayland-cursor", 11, 11},
      {XCursor, "X_cursor", 12, 12},
      {Zoom, "zoom", 9, 9},
  };
  auto description = std::ranges::find(kCursors, icon, &CursorDescription::cursor);
  return description == std::end(kCursors) ? nullptr : description;
}

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

static Str ReadThemeCursorFile(const Vec<Str>& directories, StrView theme, StrView name,
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

static Vec<CursorImage> FindNearestImages(StrView file, U32 size) {
  if (file.size() < kXcursorFileHeaderSize || !file.starts_with("Xcur")) return {};
  U32 table_position = ReadU32(file, 4);
  U32 entry_count = ReadU32(file, 12);
  if (table_position > file.size() ||
      entry_count > (file.size() - table_position) / kXcursorTableEntrySize) {
    return {};
  }
  std::optional<U32> nearest_size;
  U32 nearest_distance = 0;
  for (U32 i = 0; i < entry_count; ++i) {
    size_t entry = table_position + i * kXcursorTableEntrySize;
    if (ReadU32(file, entry) != kXcursorImageType) continue;
    U32 nominal_size = ReadU32(file, entry + 4);
    U32 distance = nominal_size > size ? nominal_size - size : size - nominal_size;
    if (!nearest_size || distance < nearest_distance) {
      nearest_size = nominal_size;
      nearest_distance = distance;
    }
  }
  if (!nearest_size) return {};
  Vec<CursorImage> images;
  for (U32 i = 0; i < entry_count; ++i) {
    size_t entry = table_position + i * kXcursorTableEntrySize;
    if (ReadU32(file, entry) != kXcursorImageType || ReadU32(file, entry + 4) != *nearest_size) {
      continue;
    }
    U32 position = ReadU32(file, entry + 8);
    if (position > file.size() || file.size() - position < kXcursorImageHeaderSize) {
      return {};
    }
    if (ReadU32(file, position + 4) != kXcursorImageType ||
        ReadU32(file, position + 8) != *nearest_size) {
      return {};
    }
    CursorImage image = {
        .width = ReadU32(file, position + 16),
        .height = ReadU32(file, position + 20),
        .xhot = ReadU32(file, position + 24),
        .yhot = ReadU32(file, position + 28),
        .delay = ReadU32(file, position + 32),
        .pixels = file.data() + position + kXcursorImageHeaderSize,
    };
    if (image.width == 0 || image.height == 0 || image.width > 0x7fff || image.height > 0x7fff ||
        U64(image.width) * image.height * 4 > file.size() - position - kXcursorImageHeaderSize) {
      return {};
    }
    images.push_back(image);
  }
  return images;
}

static std::optional<CursorImage> RenderBundledImage(StrView path,
                                                     const CursorDescription& description, U32 size,
                                                     U32 delay, SkBitmap& bitmap) {
  Status status;
  Str svg = fs::embedded.Read(Path(path), status);
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
      .xhot = U32(std::lrint(description.xhot * size / double(kBundledCursorGrid))),
      .yhot = U32(std::lrint(description.yhot * size / double(kBundledCursorGrid))),
      .delay = delay,
      .pixels = (const char*)bitmap.getPixels(),
  };
}

static Vec<CursorImage> RenderBundledCursor(const CursorDescription& description, U32 size,
                                            Vec<SkBitmap>& bitmaps) {
  if (description.frame_delays.empty()) {
    bitmaps.resize(1);
    std::optional<CursorImage> image = RenderBundledImage(
        f("assets/cursors/{}.svg", description.name), description, size, 0, bitmaps[0]);
    if (!image) return {};
    return {*image};
  }
  bitmaps.resize(description.frame_delays.size());
  Vec<CursorImage> images;
  for (size_t i = 0; i < description.frame_delays.size(); ++i) {
    std::optional<CursorImage> image =
        RenderBundledImage(f("assets/cursors/{}-{}.svg", description.name, i + 1), description,
                           size, description.frame_delays[i], bitmaps[i]);
    if (!image) return {};
    images.push_back(*image);
  }
  return images;
}

static xcb_render_pictformat_t FindArgb32Format() {
  const xcb_query_extension_reply_t* render = xcb_get_extension_data(connection, &xcb_render_id);
  if (!render || !render->present) return XCB_NONE;
  std::unique_ptr<xcb_render_query_pict_formats_reply_t, crt::DeleteWithFree> reply(
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

static xcb_cursor_t CreateCursor(xcb_render_pictformat_t argb32_format, const CursorImage& image) {
  xcb_pixmap_t pixmap = xcb_generate_id(connection);
  xcb_create_pixmap(connection, 32, pixmap, screen->root, image.width, image.height);
  xcb_gcontext_t gc = xcb_generate_id(connection);
  xcb_create_gc(connection, gc, pixmap, 0, nullptr);
  xcb_put_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, image.width, image.height, 0, 0,
                0, 32, image.width * image.height * 4, (const uint8_t*)image.pixels);
  xcb_render_picture_t picture = xcb_generate_id(connection);
  xcb_render_create_picture(connection, picture, pixmap, argb32_format, 0, nullptr);
  xcb_cursor_t cursor = xcb_generate_id(connection);
  xcb_render_create_cursor(connection, cursor, picture, image.xhot, image.yhot);
  xcb_render_free_picture(connection, picture);
  xcb_free_gc(connection, gc);
  xcb_free_pixmap(connection, pixmap);
  return cursor;
}

xcb_cursor_t LoadCursor(ui::Cursor icon) {
  const CursorDescription* description = FindCursorDescription(icon);
  if (!description) return XCB_NONE;
  xcb_render_pictformat_t argb32_format = FindArgb32Format();
  if (argb32_format == XCB_NONE) return XCB_NONE;

  Str resources = GetPropertyString(screen->root, XCB_ATOM_RESOURCE_MANAGER);
  Vec<Str> directories = CursorDirectories();
  const char* theme_variable = getenv("XCURSOR_THEME");
  StrView theme = theme_variable ? theme_variable : ResourceValue(resources, "Xcursor.theme");
  Vec<Str> searched_themes;
  Str file;
  if (!theme.empty()) {
    file = ReadThemeCursorFile(directories, theme, description->name, searched_themes);
  }
  if (file.empty()) {
    file = ReadThemeCursorFile(directories, "default", description->name, searched_themes);
  }
  U32 size = CursorSize(resources);
  Vec<SkBitmap> bitmaps;
  Vec<CursorImage> images;
  if (file.empty()) {
    images =
        RenderBundledCursor(*description, std::clamp<U32>(size, 1, kMaxBundledCursorSize), bitmaps);
  } else {
    images = FindNearestImages(file, size);
  }
  if (images.empty()) return XCB_NONE;

  if (images.size() == 1) return CreateCursor(argb32_format, images[0]);
  Vec<xcb_render_animcursorelt_t> frames;
  for (const CursorImage& image : images) {
    frames.push_back({.cursor = CreateCursor(argb32_format, image), .delay = image.delay});
  }
  xcb_cursor_t cursor = xcb_generate_id(connection);
  xcb_render_create_anim_cursor(connection, cursor, frames.size(), frames.data());
  for (const xcb_render_animcursorelt_t& frame : frames) {
    xcb_free_cursor(connection, frame.cursor);
  }
  return cursor;
}

}  // namespace xcb
