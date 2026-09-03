// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "xdg_icon.hpp"

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPictureRecorder.h>
#include <modules/svg/include/SkSVGDOM.h>

#include <cctype>
#include <cstdlib>
#include <unordered_map>

#include "format.hpp"
#include "svg.hpp"
#include "textures.hpp"
#include "vec.hpp"
#include "virtual_fs.hpp"

#pragma comment(lib, "skia")

namespace automat {
namespace {

constexpr float kDefaultSvgSize = 128;

Str Lower(StrView s) {
  Str out(s);
  for (char& c : out) c = std::tolower((unsigned char)c);
  return out;
}

const std::unordered_map<Str, Str>& ExtToMime() {
  static const auto map = [] {
    std::unordered_map<Str, Str> m;
    Status status;
    Str globs = fs::real.Read(Path("/usr/share/mime/globs2"), status);
    for (size_t i = 0; i < globs.size();) {
      size_t eol = globs.find('\n', i);
      StrView line = StrView(globs).substr(i, eol == StrView::npos ? StrView::npos : eol - i);
      i = eol == StrView::npos ? globs.size() : eol + 1;
      if (line.empty() || line[0] == '#') continue;
      size_t c1 = line.find(':');
      if (c1 == StrView::npos) continue;
      size_t c2 = line.find(':', c1 + 1);
      if (c2 == StrView::npos) continue;
      StrView mime = line.substr(c1 + 1, c2 - c1 - 1);
      StrView glob = line.substr(c2 + 1);
      if (!glob.starts_with("*.")) continue;
      StrView ext = glob.substr(2);
      if (ext.find('.') != StrView::npos || ext.find('*') != StrView::npos) continue;
      m.emplace(Lower(ext), Str(mime));
    }
    return m;
  }();
  return map;
}

const std::unordered_map<Str, Str>& MimeToGenericIcon() {
  static const auto map = [] {
    std::unordered_map<Str, Str> m;
    Status status;
    Str table = fs::real.Read(Path("/usr/share/mime/generic-icons"), status);
    for (size_t i = 0; i < table.size();) {
      size_t eol = table.find('\n', i);
      StrView line = StrView(table).substr(i, eol == StrView::npos ? StrView::npos : eol - i);
      i = eol == StrView::npos ? table.size() : eol + 1;
      size_t colon = line.find(':');
      if (colon == StrView::npos) continue;
      m.emplace(Str(line.substr(0, colon)), Str(line.substr(colon + 1)));
    }
    return m;
  }();
  return map;
}

const Vec<Str>& ThemeRoots() {
  static const Vec<Str> roots = [] {
    Vec<Str> r;
    if (const char* home = getenv("HOME")) r.push_back(Str(home) + "/.icons");
    r.push_back("/usr/share/icons/Adwaita");
    r.push_back("/usr/share/icons/gnome");
    r.push_back("/usr/share/icons/hicolor");
    return r;
  }();
  return roots;
}

Str FindIconFile(StrView icon_name) {
  for (auto& root : ThemeRoots()) {
    Str svg = f("{}/scalable/mimetypes/{}.svg", root, icon_name);
    Status status;
    if (!fs::real.Read(Path(svg), status).empty() && OK(status)) return svg;
  }
  static const char* kSizes[] = {"512x512", "256x256", "192x192", "128x128", "96x96",
                                 "64x64",   "48x48",   "32x32",   "24x24",   "16x16"};
  for (auto& root : ThemeRoots()) {
    for (const char* size : kSizes) {
      Str png = f("{}/{}/mimetypes/{}.png", root, size, icon_name);
      Status status;
      if (!fs::real.Read(Path(png), status).empty() && OK(status)) return png;
    }
  }
  return "";
}

sk_sp<SkPicture> RecordIconFile(StrView path) {
  SkPictureRecorder recorder;
  if (path.ends_with(".svg")) {
    Status status;
    Str contents = fs::real.Read(Path(Str(path)), status);
    if (contents.empty() || !OK(status)) return nullptr;
    auto dom = SVGFromAsset(contents);
    if (!dom) return nullptr;
    SkSize size = dom->containerSize();
    if (size.isEmpty()) {
      size = SkSize::Make(kDefaultSvgSize, kDefaultSvgSize);
      dom->setContainerSize(size);
    }
    dom->render(recorder.beginRecording(SkRect::MakeSize(size)));
    return recorder.finishRecordingAsPicture();
  }
  auto image = SkImages::DeferredFromEncodedData(SkData::MakeFromFileName(Str(path).c_str()));
  if (!image) return nullptr;
  auto* canvas = recorder.beginRecording(SkRect::Make(image->dimensions()));
  canvas->drawImage(image->withDefaultMipmaps(), 0, 0, kDefaultSamplingOptions);
  return recorder.finishRecordingAsPicture();
}

Vec<Str> IconCandidates(StrView mime) {
  Vec<Str> names;
  if (!mime.empty()) {
    Str exact(mime);
    for (char& c : exact)
      if (c == '/') c = '-';
    names.push_back(exact);
    if (auto it = MimeToGenericIcon().find(Str(mime)); it != MimeToGenericIcon().end()) {
      names.push_back(it->second);
    }
    size_t slash = mime.find('/');
    if (slash != StrView::npos) {
      names.push_back(f("{}-x-generic", mime.substr(0, slash)));
    }
  }
  names.push_back("text-x-generic");
  names.push_back("application-x-generic");
  return names;
}

}  // namespace

Str MimeTypeForExtension(StrView ext) {
  auto& map = ExtToMime();
  auto it = map.find(Lower(ext));
  return it == map.end() ? Str() : it->second;
}

sk_sp<SkPicture> IconForExtension(StrView ext) {
  Str mime = MimeTypeForExtension(ext);
  static std::unordered_map<Str, sk_sp<SkPicture>> cache;
  auto cached = cache.find(mime);
  if (cached != cache.end()) return cached->second;

  sk_sp<SkPicture> icon;
  for (auto& name : IconCandidates(mime)) {
    Str file = FindIconFile(name);
    if (!file.empty()) {
      icon = RecordIconFile(file);
      if (icon) break;
    }
  }
  cache[mime] = icon;
  return icon;
}

void DrawIconIn(SkCanvas& canvas, const SkPicture& icon, const Rect& box) {
  SkRect src = icon.cullRect();
  SkMatrix m = SkMatrix::RectToRect(src, box.sk, SkMatrix::kCenter_ScaleToFit);
  m.preTranslate(0, src.centerY());
  m.preScale(1, -1);
  m.preTranslate(0, -src.centerY());
  canvas.save();
  canvas.concat(m);
  canvas.drawPicture(&icon);
  canvas.restore();
}

}  // namespace automat
