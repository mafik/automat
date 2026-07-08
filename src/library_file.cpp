// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_file.hpp"

#include <fcntl.h>
#include <include/core/SkCanvas.h>
#include <sys/stat.h>

#include <cstdio>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "format.hpp"
#include "text_field.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

void RegularFile::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (!path.empty()) {
    writer.Key("path");
    writer.String(path.data(), path.size());
  }
  if (append) {
    writer.Key("append");
    writer.Bool(append);
  }
}

bool RegularFile::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "path") {
    Str new_path;
    d.Get(new_path, status);
    if (OK(status)) SetPath(new_path);
    return true;
  }
  if (key == "append") {
    bool a = false;
    d.Get(a, status);
    if (OK(status)) SetAppend(a);
    return true;
  }
  return false;
}

void RegularFile::SetPath(StrView new_path) {
  {
    auto lock = std::lock_guard(mutex);
    path = new_path;
  }
  WakeToys();
}

void RegularFile::SetAppend(bool a) {
  {
    auto lock = std::lock_guard(mutex);
    append = a;
  }
  WakeToys();
}

Str RegularFile::Path() const {
  auto lock = std::lock_guard(mutex);
  return path;
}

bool RegularFile::Append() const {
  auto lock = std::lock_guard(mutex);
  return append;
}

int RegularFile::Open(FdProvider::Dir dir, Status& status) {
  Str p;
  bool a;
  {
    auto lock = std::lock_guard(mutex);
    p = path;
    a = append;
  }
  if (p.empty()) {
    AppendErrorMessage(status) += "no path set";
    ReportError("no path set");
    return -1;
  }
  int fd;
#if defined(_WIN32)
  // _O_NOINHERIT stands in for O_CLOEXEC; binary mode keeps redirection byte-exact.
  if (dir == FdProvider::Dir::Read) {
    fd = _open(p.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  } else {
    fd = _open(p.c_str(),
               _O_WRONLY | _O_CREAT | _O_BINARY | _O_NOINHERIT | (a ? _O_APPEND : _O_TRUNC),
               _S_IREAD | _S_IWRITE);
  }
#else
  if (dir == FdProvider::Dir::Read) {
    fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
  } else {
    fd = open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (a ? O_APPEND : O_TRUNC), 0644);
  }
#endif
  if (fd < 0) {
    Str msg = f("{}: {}", p, strerror(errno));
    AppendErrorMessage(status) += msg;
    ReportError(msg);
    return -1;
  }
  ClearOwnError();
  return fd;
}

// ============================================================================
// Toy
// ============================================================================

namespace {

constexpr float kPlateW = 6.5_cm;
constexpr float kBand = ui::beta::kTitleSize + 2 * ui::beta::kPadS + 0.45_mm;
constexpr float kCreditRow = 2.0_mm;
constexpr float kSide = 2.0_mm;
constexpr float kPathRow = 6.0_mm;
constexpr float kAppendRow = 4.2_mm;
constexpr float kSizeRow = 3.2_mm;
constexpr float kTailH = 16.0_mm;
constexpr float kBottomPad = 2.2_mm;
constexpr float kPlateH =
    kBand + kCreditRow + kPathRow + kAppendRow + kSizeRow + kTailH + kBottomPad;

constexpr uint32_t kSeed = 0xF11E;

constexpr float kPathTop = kPlateH / 2 - kBand - kCreditRow;
constexpr float kAppendTop = kPathTop - kPathRow;
constexpr float kSizeTop = kAppendTop - kAppendRow;
constexpr float kTailTop = kSizeTop - kSizeRow;

constexpr int kTailLines = 7;
constexpr size_t kTailBytes = 4096;

Str Basename(StrView path) {
  auto slash = path.find_last_of("/\\");
  StrView base = slash == StrView::npos ? path : path.substr(slash + 1);
  return Str(base);
}

}  // namespace

struct FilePathField : ui::TextField {
  FilePathField(ui::Widget* parent, std::string* text, float width)
      : ui::TextField(parent, text, width) {}
  StrView Name() const override { return "FilePathField"; }
};

struct RegularFileToy : ui::beta::ObjectToy {
  std::unique_ptr<FilePathField> field;
  std::string path_edit_;

  // Tick-cached facts (UI thread only):
  Str path_applied_;
  bool append_ = false;
  bool exists_ = false;
  uint64_t size_ = 0;
  int64_t mtime_ns_ = 0;
  Vec<Str> tail_;  // the last lines of the file, ready to draw

  RegularFileToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    if (auto file = LockObject<RegularFile>()) {
      path_edit_ = file->Path();
      path_applied_ = path_edit_;
    }
    field = std::make_unique<FilePathField>(this, &path_edit_, kPlateW - 2 * kSide);
    field->local_to_parent =
        SkM44::Translate(-kPlateW / 2 + kSide, kPathTop - kPathRow) * SkM44::Scale(0.55f, 0.55f, 1);
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kPlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&RegularFile::out_stream_tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kPlateH / 2), .dir = -90_deg};
    }
    return ObjectToy::ArgStart(arg);
  }

  Rect AppendBox() const {
    return Rect{-kPlateW / 2 + kSide, kAppendTop - 3.9_mm, -kPlateW / 2 + kSide + 3.4_mm,
                kAppendTop - 0.5_mm};
  }

  // Reads the last lines of the file into tail_.
  void ReadTail() {
    tail_.clear();
    FILE* file = fopen(path_applied_.c_str(), "rb");
    if (!file) return;
    bool partial_first = size_ > kTailBytes;
    long back = (long)std::min<uint64_t>(size_, kTailBytes);
    char buf[kTailBytes];
    if (fseek(file, -back, SEEK_END) != 0) {
      fclose(file);
      return;
    }
    size_t len = fread(buf, 1, sizeof(buf), file);
    fclose(file);
    if (len == 0) return;
    Str text(buf, len);
    Vec<Str> lines;
    size_t start = 0;
    while (start <= text.size()) {
      size_t nl = text.find('\n', start);
      if (nl == Str::npos) {
        lines.push_back(text.substr(start));
        break;
      }
      lines.push_back(text.substr(start, nl - start));
      start = nl + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (partial_first && !lines.empty()) lines.erase(lines.begin());  // partial first line
    int first = std::max(0, (int)lines.size() - kTailLines);
    for (int i = first; i < (int)lines.size(); ++i) {
      Str& line = lines[i];
      for (char& c : line) {
        if (c == '\t') c = ' ';
        if ((unsigned char)c < 32) c = '.';
      }
      tail_.push_back(std::move(line));
    }
  }

  // Returns whether any drawn fact changed.
  bool UpdateFromObject() {
    bool changed = false;
    if (auto file = LockObject<RegularFile>()) {
      if (Str(path_edit_) != path_applied_) {
        path_applied_ = path_edit_;
        file->SetPath(path_applied_);
        changed = true;
      }
      bool append = file->Append();
      changed |= (append != append_);
      append_ = append;
    }
#if defined(_WIN32)
    // Whole-second mtime; sub-second rewrites are still caught through the size.
    struct _stat64 st;
    bool exists = !path_applied_.empty() && _stat64(path_applied_.c_str(), &st) == 0;
    int64_t mtime_ns = exists ? (int64_t)st.st_mtime * 1'000'000'000ll : 0;
#else
    struct stat st;
    bool exists = !path_applied_.empty() && stat(path_applied_.c_str(), &st) == 0;
    int64_t mtime_ns = exists ? st.st_mtim.tv_sec * 1'000'000'000ll + st.st_mtim.tv_nsec : 0;
#endif
    uint64_t size = exists ? (uint64_t)st.st_size : 0;
    if (exists != exists_ || size != size_ || mtime_ns != mtime_ns_) {
      exists_ = exists;
      size_ = size;
      mtime_ns_ = mtime_ns;
      if (exists_) ReadTail();
      changed = true;
    }
    return changed;
  }

  Tock Tick(time::Timer&) override {
    // The face mirrors the file on disk, so it keeps watching; it repaints
    // only when the file or the recipe moved.
    Tock tock = Tock::Ing;
    if (UpdateFromObject()) tock |= Tock::Draw;
    return tock;
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      if (AppendBox().Contains(pos)) {
        if (auto file = LockObject<RegularFile>()) file->SetAppend(!append_);
        WakeAnimation();
        return nullptr;
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void Draw(SkCanvas& canvas) const override {
    Str title = path_applied_.empty() ? Str("file") : Basename(path_applied_);
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kPlateH), title, ui::beta::kGold,
                    ui::beta::State::Default, Seed(kSeed), true);

    {  // credit: the contract this object stands for
      StrView credit = "open(2)";
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit, {kPlateW / 2 - kSide - w, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {  // stream ports on the edges the data flows through
      float w = ui::beta::TextWidth("input", ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, "input", {-w / 2, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      float ow = ui::beta::TextWidth("output", ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, "output", {-kPlateW / 2 + 10_mm - ow / 2, -kPlateH / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {  // caption over the path field
      ui::beta::DrawText(canvas, "path", {-kPlateW / 2 + kSide + 0.6_mm, kPathTop - 1.4_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {  // append: `>>` instead of `>`
      uint32_t cs = Seed(Hash2(kSeed, 0x72));
      Rect box = AppendBox();
      ui::beta::Checkbox(canvas, box, append_, ui::beta::State::Default, cs);
      ui::beta::DrawText(canvas, "append", {box.right + 1.5_mm, box.bottom + 0.9_mm},
                         ui::beta::kMicroSize, ui::beta::kInk, false, cs);
    }

    {  // size readout, or the file's absence
      Str label = !exists_ ? Str("missing") : FormatBytes(size_);
      SkColor color = exists_ ? ui::beta::kInk : ui::beta::kGrayDark;
      ui::beta::DrawText(canvas, label, {-kPlateW / 2 + kSide, kSizeTop - 2.6_mm},
                         ui::beta::kMicroSize, color, false, Seed(kSeed));
    }

    {  // the tail of the content, like a terminal
      Rect box{-kPlateW / 2 + kSide, kTailTop - kTailH, kPlateW / 2 - kSide, kTailTop};
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(box.sk, bg);
      float line_h = 2.2_mm;
      float y = box.bottom + 0.7_mm;
      canvas.save();
      canvas.clipRect(box.sk);
      for (int i = (int)tail_.size() - 1; i >= 0; --i) {
        ui::beta::DrawText(canvas, tail_[i], {box.left + 0.8_mm, y}, ui::beta::kMicroSize,
                           ui::beta::kPaper, false, Seed(kSeed));
        y += line_h;
      }
      canvas.restore();
      SkPaint frame;
      frame.setStyle(SkPaint::kStroke_Style);
      frame.setStrokeWidth(ui::beta::kStroke * 0.8f);
      frame.setColor(ui::beta::kInk);
      canvas.drawRect(box.sk, frame);
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> RegularFile::MakeToy(ui::Widget* parent) {
  return std::make_unique<RegularFileToy>(parent, *this);
}

}  // namespace automat::library
