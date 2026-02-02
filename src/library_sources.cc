// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_sources.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>

#include "action.hh"
#include "embedded.hh"
#include "log.hh"
#include "path.hh"
#include "textures.hh"
#include "virtual_fs.hh"

using namespace std;

namespace automat::library {

// Image dimensions: 778x1008 pixels
// Aspect ratio: 778/1008 ≈ 0.772
constexpr float kSourcesHeight = 10_cm;
constexpr float kSourcesWidth = kSourcesHeight * 778.f / 1008.f;

static PersistentImage& SourcesImage() {
  static auto image =
      PersistentImage::MakeFromAsset(embedded::assets_sources_webp, {.height = kSourcesHeight});
  return image;
}

Sources::Sources() {}

std::string_view Sources::Name() const { return "Sources"; }

Ptr<Object> Sources::Clone() const { return MAKE_PTR(Sources); }

struct ExtractFilesOption : TextOption {
  WeakPtr<Sources> weak;

  ExtractFilesOption(WeakPtr<Sources> weak) : TextOption("Extract Files"), weak(weak) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<ExtractFilesOption>(weak);
  }

  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    // Extract all embedded files to the current directory
    Status status;
    int file_count = 0;
    for (auto& [path, vfile] : embedded::index) {
      Path out_path(path);
      // Create parent directories if needed
      auto parent = out_path.Parent();
      if (!parent.str.empty()) {
        parent.MakeDirs(status);
        if (!OK(status)) {
          LOG << "Failed to create directory for " << path << ": " << status;
          status = Status();
          continue;
        }
      }
      fs::real.Write(out_path, vfile->content, status);
      if (!OK(status)) {
        LOG << "Failed to extract " << path << ": " << status;
        status = Status();
        continue;
      }
      ++file_count;
    }
    LOG << "Extracted " << file_count << " files";
    return nullptr;
  }

  Dir PreferredDir() const override { return SW; }
};

struct SourcesWidget : Object::WidgetBase {
  SourcesWidget(ui::Widget* parent, Object& sources) : WidgetBase(parent, sources) {}

  Ptr<Sources> LockSources() const { return LockObject<Sources>(); }

  static Rect GetRect() { return Rect::MakeCornerZero(kSourcesWidth, kSourcesHeight); }

  SkPath Shape() const override { return SkPath::Rect(GetRect()); }

  RRect CoarseBounds() const override { return RRect::MakeSimple(GetRect(), 0); }

  void Draw(SkCanvas& canvas) const override { SourcesImage().draw(canvas); }

  void VisitOptions(const OptionsVisitor& visitor) const override {
    WidgetBase::VisitOptions(visitor);
    if (auto sources = LockSources()) {
      ExtractFilesOption extract(sources);
      visitor(extract);
    }
  }
};

std::unique_ptr<Toy> Sources::MakeToy(ui::Widget* parent, ReferenceCounted&) {
  return std::make_unique<SourcesWidget>(parent, *this);
}

void Sources::SerializeState(ObjectSerializer& writer) const {
  // No state to serialize
}

bool Sources::DeserializeKey(ObjectDeserializer& d, StrView key) {
  // No state to deserialize
  return false;
}

}  // namespace automat::library
