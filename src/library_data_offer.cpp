// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#if defined(_WIN32)
#undef NOGDI
// clang-format off
#include <windows.h>
// clang-format on

#include <cguid.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj_core.h>
#include <wrl/client.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#endif

#include <include/core/SkCanvas.h>

#include <unordered_map>

#include "board.hpp"
#include "drag_action.hpp"
#include "engine.hpp"
#include "format.hpp"
#include "library_data_offer.hpp"
#include "library_file.hpp"
#include "location.hpp"
#include "object.hpp"
#include "root_widget.hpp"
#include "tasks.hpp"
#include "textures.hpp"
#include "ui_beta.hpp"
#include "units.hpp"
#include "virtual_fs.hpp"
#include "xdg_icon.hpp"

#if defined(__linux__)
#include "xcb.hpp"
#endif

#if defined(_WIN32)
#include "win32.hpp"
#endif

#pragma comment(lib, "skia")

namespace automat::library {

using namespace ui::beta;

static SkPaint Fill(SkColor c) {
  SkPaint p;
  p.setAntiAlias(true);
  p.setColor(c);
  return p;
}

static DragLocationAction* DragOf(ui::Pointer& mouse) {
  return dynamic_cast<DragLocationAction*>(
      mouse.actions[static_cast<int>(ui::PointerButton::Left)].get());
}

static void PostTryFinishDrop(DataOffer& offer) {
  ui::root_widget->window->RunOnMainLoop([weak = offer.AcquireWeakPtr()] {
    auto offer = weak.Lock();
    if (!offer) return;
    auto lock = ui::root_widget->window->Lock();
    offer->TryFinishDrop();
  });
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

static IGlobalInterfaceTable& GlobalInterfaceTable() {
  static IGlobalInterfaceTable* table = [] {
    IGlobalInterfaceTable* table = nullptr;
    CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&table));
    return table;
  }();
  return *table;
}

static ComPtr<IDataObject> Source(DataOffer& offer, Status& status) {
  ComPtr<IDataObject> data;
  HRESULT hr = GlobalInterfaceTable().GetInterfaceFromGlobal(
      offer.source_cookie.load(std::memory_order_relaxed), IID_PPV_ARGS(&data));
  if (FAILED(hr)) AppendErrorMessage(status) += f("GetInterfaceFromGlobal: {:#x}", (uint32_t)hr);
  return data;
}

static void ReleaseSource(DataOffer& offer) {
  unsigned long cookie = offer.source_cookie.exchange(0, std::memory_order_relaxed);
  if (!cookie) return;
  bool end_operation = std::exchange(offer.async_started, false);
  ui::root_widget->window->RunOnMainLoop([cookie, end_operation] {
    if (end_operation) {
      ComPtr<IDataObjectAsyncCapability> async;
      if (SUCCEEDED(GlobalInterfaceTable().GetInterfaceFromGlobal(cookie, IID_PPV_ARGS(&async)))) {
        async->EndOperation(S_OK, nullptr, DROPEFFECT_COPY);
      }
    }
    GlobalInterfaceTable().RevokeInterfaceFromGlobal(cookie);
  });
}

static void ScheduleRead(DataOffer& offer, DataOffer::Entry& e, FORMATETC format) {
  (new FunctionTask(offer.AcquireWeakPtr(), [&e, format](Object& o) {
    auto& offer = static_cast<DataOffer&>(o);
    if (offer.cancelled.load(std::memory_order_relaxed)) return;
    Status status;
    ComPtr<IDataObject> data = Source(offer, status);
    FORMATETC request = format;
    STGMEDIUM medium = {};
    if (data) {
      HRESULT hr = data->GetData(&request, &medium);
      if (FAILED(hr)) {
        AppendErrorMessage(status) +=
            f("GetData(format {}): {:#x}", request.cfFormat, (uint32_t)hr);
      }
    }
    if (OK(status) && medium.tymed == TYMED_HGLOBAL) {
      auto* bytes = (const char*)GlobalLock(medium.hGlobal);
      if (!bytes) {
        AppendErrorMessage(status) += f("GlobalLock: {}", win32::GetLastErrorStr());
      } else {
        StrView dib(bytes, GlobalSize(medium.hGlobal));
        Str file;
        if ((request.cfFormat == CF_DIB || request.cfFormat == CF_DIBV5) &&
            dib.size() >= sizeof(BITMAPINFOHEADER)) {
          auto& info = *(const BITMAPINFOHEADER*)dib.data();
          uint32_t colors = info.biClrUsed;
          if (colors == 0 && info.biBitCount <= 8) colors = 1u << info.biBitCount;
          uint32_t masks =
              info.biSize == sizeof(BITMAPINFOHEADER) && info.biCompression == BI_BITFIELDS ? 12
                                                                                            : 0;
          BITMAPFILEHEADER header = {
              .bfType = 0x4D42,
              .bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) + dib.size()),
              .bfOffBits = (DWORD)(sizeof(BITMAPFILEHEADER) + info.biSize + masks +
                                   colors * sizeof(RGBQUAD)),
          };
          file.assign((const char*)&header, sizeof(header));
        }
        file += dib;
        GlobalUnlock(medium.hGlobal);
        e.bytes_total.store(file.size(), std::memory_order_relaxed);
        e.file->Append(file);
        e.bytes_done.store(file.size(), std::memory_order_relaxed);
      }
    } else if (OK(status) && medium.tymed == TYMED_ISTREAM) {
      IStream& stream = *medium.pstm;
      if (e.bytes_total.load(std::memory_order_relaxed) == 0) {
        STATSTG stat;
        if (SUCCEEDED(stream.Stat(&stat, STATFLAG_NONAME))) {
          e.bytes_total.store(stat.cbSize.QuadPart, std::memory_order_relaxed);
        }
      }
      char buf[1 << 16];
      uint64_t done = 0;
      while (!offer.cancelled.load(std::memory_order_relaxed)) {
        ULONG n = 0;
        HRESULT hr = stream.Read(buf, sizeof(buf), &n);
        if (FAILED(hr)) {
          AppendErrorMessage(status) += f("IStream::Read: {:#x}", (uint32_t)hr);
          break;
        }
        if (n == 0) break;
        e.file->Append(StrView(buf, n));
        done += n;
        e.bytes_done.store(done, std::memory_order_relaxed);
      }
    } else if (OK(status)) {
      AppendErrorMessage(status) += f("Unsupported storage medium {}", medium.tymed);
    }
    ReleaseStgMedium(&medium);
    offer.EntryWriteOver(e, status);
  }))->Schedule();
}

bool DataOffer::Receive(IDataObject& data) {
  static CLIPFORMAT descriptor = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
  static CLIPFORMAT contents = RegisterClipboardFormat(CFSTR_FILECONTENTS);
  static CLIPFORMAT png = RegisterClipboardFormat(TEXT("PNG"));
  auto offered = [&](CLIPFORMAT format) {
    FORMATETC request = {format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data.QueryGetData(&request) == S_OK;
  };
  std::function<void(DataOffer&)> describe;
  if (offered(CF_HDROP)) {
    describe = [](DataOffer& offer) {
      Status status;
      if (auto data = Source(offer, status)) {
        FORMATETC request = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        HRESULT hr = data->GetData(&request, &medium);
        if (FAILED(hr)) {
          AppendErrorMessage(status) += f("GetData(CF_HDROP): {:#x}", (uint32_t)hr);
        } else {
          auto drop = (HDROP)medium.hGlobal;
          UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
          for (UINT i = 0; i < count; ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            path.resize(DragQueryFileW(drop, i, path.data(), path.size()));
            offer.AddPath(Path(win32::WideToUtf8(path)));
          }
          ReleaseStgMedium(&medium);
        }
      }
      if (!OK(status)) offer.ReportError(status.ToStr());
    };
  } else if (offered(descriptor)) {
    describe = [](DataOffer& offer) {
      Status status;
      if (auto data = Source(offer, status)) {
        FORMATETC request = {descriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        HRESULT hr = data->GetData(&request, &medium);
        if (FAILED(hr)) {
          AppendErrorMessage(status) += f("GetData(FileGroupDescriptorW): {:#x}", (uint32_t)hr);
        } else {
          auto* group = (const FILEGROUPDESCRIPTORW*)GlobalLock(medium.hGlobal);
          for (UINT i = 0; group && i < group->cItems; ++i) {
            const FILEDESCRIPTORW& d = group->fgd[i];
            if ((d.dwFlags & FD_ATTRIBUTES) && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
              continue;
            }
            auto* e = offer.Add(Path(win32::WideToUtf8(d.cFileName)).Name());
            if (!e) break;
            if (d.dwFlags & FD_FILESIZE) {
              e->bytes_total.store((uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow,
                                   std::memory_order_relaxed);
            }
            ScheduleRead(
                offer, *e,
                {contents, nullptr, DVASPECT_CONTENT, (LONG)i, TYMED_ISTREAM | TYMED_HGLOBAL});
          }
          GlobalUnlock(medium.hGlobal);
          ReleaseStgMedium(&medium);
        }
      }
      if (!OK(status)) offer.ReportError(status.ToStr());
    };
  } else {
    CLIPFORMAT bitmap = offered(png)        ? png
                        : offered(CF_DIBV5) ? CF_DIBV5
                        : offered(CF_DIB)   ? CF_DIB
                                            : 0;
    if (!bitmap) return false;
    describe = [bitmap](DataOffer& offer) {
      bool dib = bitmap == CF_DIB || bitmap == CF_DIBV5;
      if (auto* e = offer.Add(dib ? "image.bmp" : "image.png")) {
        ScheduleRead(offer, *e, {bitmap, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
      }
    };
  }
  unsigned long cookie = 0;
  HRESULT hr =
      GlobalInterfaceTable().RegisterInterfaceInGlobal(&data, __uuidof(IDataObject), &cookie);
  if (FAILED(hr)) {
    ReportError(f("RegisterInterfaceInGlobal: {:#x}", (uint32_t)hr));
    sealed.store(true, std::memory_order_release);
    return true;
  }
  source_cookie.store(cookie, std::memory_order_relaxed);
  (new FunctionTask(AcquireWeakPtr(), [describe = std::move(describe)](Object& o) {
    auto& offer = static_cast<DataOffer&>(o);
    if (offer.cancelled.load(std::memory_order_relaxed)) return;
    describe(offer);
    offer.sealed.store(true, std::memory_order_release);
    PostTryFinishDrop(offer);
  }))->Schedule();
  return true;
}

DataOffer::~DataOffer() { ReleaseSource(*this); }

#endif

DataOffer::Entry* DataOffer::Add(StrView name) {
  uint32_t n = entry_count.load(std::memory_order_relaxed);
  if (n >= kMaxEntries) return nullptr;
  entries[n].name = Str(name);
  entries[n].file = MAKE_PTR(File, name);
  entry_count.store(n + 1, std::memory_order_release);
  WakeToys();
  return &entries[n];
}

void DataOffer::AddPath(const Path& source) {
  Entry* e = Add(source.Name());
  if (!e) return;
  e->source_path = source;
  (new FunctionTask(AcquireWeakPtr(), [e](Object& o) {
    auto& offer = static_cast<DataOffer&>(o);
    if (offer.cancelled.load(std::memory_order_relaxed)) return;
    Status status;
    fs::real.Map(
        e->source_path,
        [&](StrView content) {
          e->bytes_total.store(content.size(), std::memory_order_relaxed);
          while (!content.empty() && !offer.cancelled.load(std::memory_order_relaxed)) {
            StrView slice = content.substr(0, 1 << 16);
            e->file->Append(slice);
            content.remove_prefix(slice.size());
            e->bytes_done.fetch_add(slice.size(), std::memory_order_relaxed);
          }
        },
        status);
    offer.EntryWriteOver(*e, status);
  }))->Schedule();
}

void DataOffer::EntryWriteOver(Entry& e, Status& status) {
  if (cancelled.load(std::memory_order_relaxed)) return;
  if (OK(status)) e.file->FlushToFilesystem(status);
  if (!OK(status)) {
    e.failed.store(true, std::memory_order_relaxed);
    ReportError(status.ToStr());
  }
  e.done.store(true, std::memory_order_release);
  WakeToys();
  PostTryFinishDrop(*this);
}

bool DataOffer::Done() const {
  if (!sealed.load(std::memory_order_acquire)) return false;
  int n = EntryCount();
  for (int i = 0; i < n; ++i) {
    if (!entries[i].done.load(std::memory_order_acquire)) return false;
  }
  return true;
}

bool DataOffer::Accepted() const {
  int n = EntryCount();
  for (int i = 0; i < n; ++i) {
    if (entries[i].done.load(std::memory_order_acquire) &&
        !entries[i].failed.load(std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void DataOffer::BeginDragAction(ui::Pointer& mouse) {
  auto home = MAKE_PTR(Location);
  home->InsertHere(AcquirePtr());
  home->placement = Location::Direct{mouse.PositionOnCanvas()};
  mouse.actions[static_cast<int>(ui::PointerButton::Left)] =
      std::make_unique<DragLocationAction>(mouse, std::move(home));
}

void DataOffer::CancelDragAction(ui::Pointer& mouse) {
  cancelled.store(true, std::memory_order_relaxed);
  while (auto* drag = DragOf(mouse)) {
    if (!drag->RemoveFromGroup(*drag->locations.front())) break;
  }
#if defined(_WIN32)
  ReleaseSource(*this);
#endif
}

void DataOffer::EndDragAction(ui::Pointer& mouse) {
  dropped = true;
  mouse.ButtonUp(ui::PointerButton::Left);
#if defined(_WIN32)
  if (!Done()) {
    ComPtr<IDataObjectAsyncCapability> async;
    BOOL async_mode = FALSE;
    if (SUCCEEDED(GlobalInterfaceTable().GetInterfaceFromGlobal(
            source_cookie.load(std::memory_order_relaxed), IID_PPV_ARGS(&async))) &&
        SUCCEEDED(async->GetAsyncMode(&async_mode)) && async_mode &&
        SUCCEEDED(async->StartOperation(nullptr))) {
      async_started = true;
    }
  }
#endif
  TryFinishDrop();
}

void DataOffer::TryFinishDrop() {
  if (cancelled.load(std::memory_order_relaxed)) return;
  Ptr<Location> home = HomeLocation();
  ui::Pointer* mouse = ui::root_widget->window->MouseOrNull();
  DragLocationAction* drag = mouse ? DragOf(*mouse) : nullptr;
  if (drag && !drag->locations.Contains(home)) drag = nullptr;
  int n = EntryCount();
  for (int i = 0; i < n; ++i) {
    Entry& e = entries[i];
    if (!e.file || !e.done.load(std::memory_order_acquire) ||
        e.failed.load(std::memory_order_relaxed)) {
      continue;
    }
    auto file = std::move(e.file);
    file->Analyze();
    auto loc = MAKE_PTR(Location);
    loc->InsertHere(std::move(file));
    if (drag) {
      drag->AddToGroup(std::move(loc));
    } else if (auto board = home ? home->LockBoard() : nullptr) {
      loc->board = board->AcquireWeakPtr();
      loc->placement = Location::Direct{home->PeekPosition() + Vec2(6_mm, -6_mm)};
      {
        auto lock = std::lock_guard(engine.mutex);
        board->locations.emplace(board->locations.begin(), *board, loc);
      }
      loc->WakeToys();
    }
  }
  if (!Done()) return;
  if (home && !HasError(*this)) {
    if (!(drag && drag->RemoveFromGroup(*home))) {
      if (auto board = home->LockBoard()) board->Extract(*home);
    }
  }
  if (!dropped) return;
#if defined(_WIN32)
  ReleaseSource(*this);
#endif
#if defined(__linux__)
  if (!source) return;
  bool accepted = Accepted();
  uint32_t reply[5] = {target, accepted ? 1u : 0u, accepted ? action : 0u, 0, 0};
  xcb_window_t to = source;
  source = 0;
  if (accepted && action == xcb::atom::XdndActionMove) {
    (new FunctionTask(AcquireWeakPtr(), [self = AcquirePtr(), reply, to](Object&) {
      int n = self->EntryCount();
      for (int i = 0; i < n; ++i) {
        auto& e = self->entries[i];
        if (e.failed.load(std::memory_order_relaxed) || e.source_path.str.empty()) continue;
        Status status;
        e.source_path.Unlink(status, true);
      }
      xcb::SendClientMessage(to, xcb::atom::XdndFinished, reply);
    }))->Schedule();
  } else {
    xcb::SendClientMessage(to, xcb::atom::XdndFinished, reply);
  }
#endif
}

namespace {

constexpr float kCard = 2.4_cm;
constexpr float kIconInset = 3_mm;
constexpr float kRingR = kCard * 0.62f;

struct Row {
  Str name;
  float progress;  // -1 indeterminate, else [0, 1]
};

Str ExtensionOf(StrView name) {
  auto dot = name.rfind('.');
  return dot == StrView::npos ? Str() : Str(name.substr(dot + 1));
}

}  // namespace

struct DataOfferToy : automat::ObjectToy {
  Vec<Row> rows;
  bool animating = false;
  float phase = 0;
  mutable std::unordered_map<Str, sk_sp<SkPicture>> icon_cache;

  DataOfferToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) { Pull(); }

  bool CenteredAtZero() const override { return true; }

  void Pull() {
    rows.clear();
    animating = false;
    if (auto offer = LockObject<DataOffer>()) {
      int n = offer->EntryCount();
      for (int i = 0; i < n; ++i) {
        auto& e = offer->entries[i];
        bool done = e.done.load(std::memory_order_acquire);
        float progress;
        if (done) {
          progress = 1;
        } else {
          uint64_t total = e.bytes_total.load(std::memory_order_relaxed);
          uint64_t got = e.bytes_done.load(std::memory_order_relaxed);
          progress = total ? (float)((double)got / (double)total) : -1;
          animating = true;
        }
        rows.push_back({e.name, progress});
      }
      if (n == 0) animating = true;
    }
  }

  const sk_sp<SkPicture>& IconFor(const Str& name) const {
    Str ext = ExtensionOf(name);
    auto it = icon_cache.find(ext);
    if (it == icon_cache.end()) it = icon_cache.emplace(ext, IconForExtension(ext)).first;
    return it->second;
  }

  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kCard, kCard), 1.5_mm).sk);
  }
  Optional<Rect> DrawBounds() const override {
    return Rect::MakeCenterZero(kCard, kCard).Outset(8_mm);
  }

  Tock Tick(time::Timer& timer) override {
    Pull();
    if (animating) {
      phase = (float)std::fmod(timer.NowSeconds() * 0.8, 1.0);
      return Tock::Drawing;
    }
    return Tock::Draw;
  }

  void DrawCard(SkCanvas& canvas, Vec2 c, const Row& row, float shade, bool front) const {
    uint32_t seed = Hash2(ID(), (uint32_t)std::lround(c.x * 1000));
    Rect card = Rect::MakeCenter(c, kCard, kCard);
    SkPath body = WonkyRoundRect(card, 1.2_mm, kWonk, seed);
    HandShadow(canvas, body, {kShadowDX, -kShadowDY}, kShadow, seed);
    canvas.drawPath(body, Fill(shade < 1 ? 0xffdcdcdc : kPaper));
    if (auto& icon = IconFor(row.name)) DrawIconIn(canvas, *icon, card.Inset(kIconInset));
    SketchyStroke(canvas, body, kInk, kStroke, seed, 2);
    if (front) DrawRing(canvas, c, row.progress, seed);
  }

  void DrawRing(SkCanvas& canvas, Vec2 c, float progress, uint32_t seed) const {
    SkRect oval = Rect::MakeCenter(c, kRingR * 2, kRingR * 2).sk;
    canvas.drawArc(oval, 0, 360, false, InkPaint(kGray, kStrokeBold));
    SkPaint arc = InkPaint(kGreen, kStrokeBold);
    if (progress < 0) {
      canvas.drawArc(oval, phase * 360.f, 90, false, arc);
    } else {
      canvas.drawArc(oval, -90, progress * 360.f, false, arc);
    }
  }

  void Draw(SkCanvas& canvas) const override {
    if (rows.empty()) {
      Rect card = Rect::MakeCenterZero(kCard, kCard);
      SketchyStroke(canvas, WonkyRoundRect(card, 1.2_mm, kWonk, ID()), kInkSoft, kStroke, ID(), 2);
      DrawRing(canvas, {0, 0}, -1, ID());
      return;
    }
    int shown = std::min<int>(rows.size(), 3);
    for (int i = shown - 1; i >= 1; --i) {
      DrawCard(canvas, Vec2(i * 2.2_mm, i * -1.6_mm), rows[i], 0.85f, false);
    }
    DrawCard(canvas, {0, 0}, rows[0], 1.f, true);
    DrawBetaStamp(canvas, {kCard / 2 - 0.58_mm, kCard / 2 - 0.875_mm}, 3.1_mm, -12.f,
                  Hash2(ID(), 12u));
    if (rows.size() > 3) {
      DrawText(canvas, f("+{}", rows.size() - 3), {kCard / 2 - 6_mm, -kCard / 2 + 1_mm}, 3_mm, kInk,
               true, ID());
    }
  }
};

std::unique_ptr<automat::ObjectToy> DataOffer::MakeToy(ui::Widget* parent) {
  return std::make_unique<DataOfferToy>(parent, *this);
}

}  // namespace automat::library
