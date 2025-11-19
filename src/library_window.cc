// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_window.hh"

#include <include/core/SkColor.h>
#include <include/core/SkColorType.h>
#include <include/core/SkImage.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>

#include <tracy/Tracy.hpp>

#include "argument.hh"
#include "color.hh"
#include "font.hh"
#include "key.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "str.hh"
#include "svg.hh"
#include "textures.hh"
#include "theme_xp.hh"
#include "time.hh"
#include "ui_shape_widget.hh"

#ifdef __linux__
#include <sys/shm.h>
#include <xcb/shm.h>

#include <ranges>

#include "control_flow.hh"
#include "xcb.hh"
#endif

#ifdef _WIN32
#undef NOGDI
#include <dwmapi.h>
#include <windows.h>

#include <vector>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// Windows helper functions
static std::string GetWindowTitle(HWND hwnd) {
  int length = GetWindowTextLengthA(hwnd);
  if (length == 0) return "";

  std::string title(length, '\0');
  GetWindowTextA(hwnd, title.data(), length + 1);
  return title;
}

static bool IsValidWindow(HWND hwnd) {
  if (!IsWindow(hwnd)) return false;
  if (!IsWindowVisible(hwnd)) return false;

  // Skip windows with no title
  if (GetWindowTitle(hwnd).empty()) return false;

  // Skip certain window classes
  char className[256];
  if (GetClassNameA(hwnd, className, sizeof(className))) {
    std::string classStr(className);
    if (classStr == "Shell_TrayWnd" || classStr == "DV2ControlHost" ||
        classStr == "MsgrIMEWindowClass" || classStr == "SysShadow") {
      return false;
    }
  }

  return true;
}
#endif

using namespace std;

namespace automat::library {

constexpr bool kDebugWindowPicking = false;

struct EnableContinuousRunOption : TextOption {
  WeakPtr<Window> weak;

  EnableContinuousRunOption(WeakPtr<Window> weak) : TextOption("Start"), weak(weak) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<EnableContinuousRunOption>(weak);
  }

  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto window = weak.lock()) {
      auto lock = std::lock_guard(window->mutex);
      window->run_continuously = true;
      // Start continuous execution
      if (auto here_ptr = window->here.lock()) {
        here_ptr->ScheduleRun();
      }
    }
    return nullptr;
  }
  Dir PreferredDir() const override { return SW; }
};

struct DisableContinuousRunOption : TextOption {
  WeakPtr<Window> weak;

  DisableContinuousRunOption(WeakPtr<Window> weak) : TextOption("Stop"), weak(weak) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<DisableContinuousRunOption>(weak);
  }

  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto window = weak.lock()) {
      auto lock = std::lock_guard(window->mutex);
      window->run_continuously = false;
    }
    return nullptr;
  }
};

std::string_view Window::Name() const { return "Window"; }

struct Window::Impl {
#ifdef __linux__
  xcb_window_t xcb_window = XCB_WINDOW_NONE;

  xcb_shm_seg_t shmseg = -1;
  int shmid = -1;
  std::span<char> data;

  ~Impl() {
    if (shmseg != -1) {  // Only cleanup if initialized
      xcb_shm_detach(xcb::connection, shmseg);

      if (data.data()) {
        shmdt(data.data());
      }
      if (shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
      }
    }
  }
#endif

#ifdef _WIN32
  HWND hwnd = nullptr;
#endif

  Impl() {}
};

Window::Window() { impl = std::make_unique<Impl>(); }

Ptr<Object> Window::Clone() const {
  auto ret = MAKE_PTR(Window);
  ret->run_continuously = run_continuously;
  ret->captured_image = captured_image;
  ret->capture_time = capture_time;
#ifdef __linux__
  ret->impl->xcb_window = impl->xcb_window;
#endif
#ifdef _WIN32
  ret->impl->hwnd = impl->hwnd;
#endif
  return ret;
}

constexpr static float kBorderWidth = theme::xp::kBorderWidth;  // width of the border
constexpr static float kContentMargin =
    theme::xp::kBorderWidth;  // margin between the border and the content
constexpr static float kTitleHeight = theme::xp::kTitleBarHeight;  // height of the title bar
constexpr static float kTitleButtonSize = kTitleHeight - 2 * kContentMargin;

struct WindowWidget;

struct PickButton : theme::xp::TitleButton {
  std::function<void(ui::Pointer&)> on_activate;
  PickButton(ui::Widget* parent) : theme::xp::TitleButton(parent) {
    child = ui::MakeShapeWidget(this, kPickSVG, "#000000"_color);
    UpdateChildTransform();
    child->local_to_parent.preTranslate(-0.6_mm, 0.6_mm);
  }
  // float Height() const override { return kTitleButtonSize; }
  SkRRect RRect() const override {
    return RRect::MakeSimple(
               Rect::MakeAtZero<::LeftX, ::BottomY>({kTitleButtonSize, kTitleButtonSize}), 2_mm)
        .sk;
  }

  void Activate(ui::Pointer& p) override {
    WakeAnimation();
    on_activate(p);
  }
};

#ifdef __linux__
using WindowVisitor = std::function<ControlFlow(xcb_window_t window, xcb_window_t parent)>;
static bool HasWMState(xcb_window_t window) {
  auto property_reply = xcb::get_property(window, xcb::atom::WM_STATE, XCB_ATOM_ANY, 0, 0);
  return property_reply->type != XCB_ATOM_NONE;
}
static void SearchWindows(xcb_window_t start, WindowVisitor visitor) {
  std::deque<std::pair<xcb_window_t, xcb_window_t>> search_list;
  search_list.push_back({start, XCB_WINDOW_NONE});
  while (!search_list.empty()) {
    // Pick the front window, search its children and if none of them has WM_STATE, add them to
    // the queue.
    auto curr = search_list.front();
    search_list.pop_front();
    if (curr.first == XCB_WINDOW_NONE) {
      continue;
    }
    auto control_flow = visitor(curr.first, curr.second);
    if (control_flow == ControlFlow::StopSearching) {
      break;
    } else if (control_flow == ControlFlow::SkipChildren) {
      continue;
    }
    auto query_tree_reply = xcb::query_tree(curr.first);
    for (auto child : std::ranges::reverse_view(xcb::query_tree_children(*query_tree_reply))) {
      search_list.push_back({child, curr.first});
    }
  }
}
#endif

struct WindowWidget : Object::WidgetBase, ui::PointerGrabber, ui::KeyGrabber {
  constexpr static float kWidth = 5_cm;
  constexpr static float kCornerRadius = 1_mm;
  constexpr static float kHeight = 5_cm;

  ui::PointerGrab* pointer_grab = nullptr;
  ui::KeyGrab* key_grab = nullptr;

  std::unique_ptr<PickButton> pick_button;
  std::string window_name;

  sk_sp<SkImage> captured_image;  // Local copy of the captured bitmap
  SkColor title_bar_color = "#0066ff"_color;

  Ptr<Window> LockWindow() const { return LockObject<Window>(); }

  WindowWidget(ui::Widget* parent, WeakPtr<Object> window) : WidgetBase(parent) {
    object = window;
    pick_button = std::make_unique<PickButton>(this);
    pick_button->on_activate = [this](ui::Pointer& p) {
      p.EndAllActions();
      pointer_grab = &p.RequestGlobalGrab(*this);
      if (p.keyboard) {
        key_grab = &p.keyboard->RequestKeyGrab(*this, ui::AnsiKey::Escape, false, false, false,
                                               false, [this](Status& status) {
                                                 if (!OK(status)) {
                                                   LOG << "Couldn't grab the escape key:" << status;
                                                   ReleaseGrabs();
                                                 }
                                               });
      }
    };
    auto content_bounds = kCoarseBounds.Outset(-kBorderWidth - kContentMargin);
    auto title_bounds = Rect(kCoarseBounds.rect.left, kCoarseBounds.rect.top - kTitleHeight,
                             kCoarseBounds.rect.right, kCoarseBounds.rect.top);

    auto pos = title_bounds.RightCenter();
    pos.x -= kTitleButtonSize + kContentMargin;
    pos.y -= kTitleButtonSize / 2;
    pick_button->local_to_parent = SkM44::Translate(pos.x, pos.y);
  }

  constexpr static RRect kCoarseBounds =
      RRect::MakeWithRadii(Rect::MakeAtZero({kWidth, kHeight}), theme::xp::kTitleCornerRadius,
                           theme::xp::kTitleCornerRadius, 0, 0);
  constexpr static RRect kBorderInner = kCoarseBounds.Outset(-kBorderWidth);
  constexpr static float kRegionStrokeWidth = 1_mm;

  RRect CoarseBounds() const override { return kCoarseBounds; }

  SkPath Shape() const override { return SkPath::RRect(CoarseBounds().sk); }

  animation::Phase Tick(time::Timer& timer) override {
    auto window = LockWindow();
    auto lock = std::lock_guard(window->mutex);
    if (window_name != window->title) {
      window_name = window->title;
    }
    // Copy the captured image from the Window object
    captured_image = window->captured_image;

    // Compute title bar color decay from blue to silver
    float t =
        std::clamp<float>((timer.NowSeconds() - window->capture_time - timer.d * 2) / 0.3, 0, 1);

    // Interpolate from blue (#0078d4) to silver (#c0c0c0)
    SkColor blue = "#0066ff"_color;
    SkColor silver = "#bbbccc"_color;

    title_bar_color = color::MixColors(blue, silver, t);

    // Continue animation if not fully decayed
    if (t < 1.0f) {
      return animation::Animating;
    }

    return animation::Finished;
  }

  struct LayoutData {
    RRect contents_rrect;
    Rect title_rect;
    Rect full_region_rect;
    SkMatrix image_matrix;
  };

  LayoutData Layout() const {
    LayoutData l = {};
    l.title_rect = Rect(kCoarseBounds.rect.left, kCoarseBounds.rect.top - kTitleHeight,
                        kCoarseBounds.rect.right, kCoarseBounds.rect.top);
    l.contents_rrect = kBorderInner;
    l.contents_rrect.rect.top = l.title_rect.bottom;
    if (captured_image) {
      SkRect image_rect = SkRect::Make(captured_image->dimensions());
      l.image_matrix =
          SkMatrix::RectToRect(image_rect, l.contents_rrect.rect, SkMatrix::kCenter_ScaleToFit);
      l.image_matrix.preTranslate(0, captured_image->height() / 2.f);
      l.image_matrix.preScale(1, -1);
      l.image_matrix.preTranslate(0, -captured_image->height() / 2.f);
      l.full_region_rect = image_rect;
      l.image_matrix.mapRect(&l.full_region_rect.sk);
    } else {
      l.full_region_rect = l.contents_rrect.rect;
    }
    return l;
  }

  void Draw(SkCanvas& canvas) const override {
    auto layout = Layout();

    auto vertices = theme::xp::WindowBorder(kCoarseBounds.rect, title_bar_color);
    canvas.drawVertices(vertices, SkBlendMode::kDst, SkPaint());

    auto& font = ui::GetFont();
    SkPaint title_text_paint;
    title_text_paint.setColor("#ffffff"_color);
    canvas.save();
    auto title_text_pos = layout.title_rect.LeftCenter();
    title_text_pos.x += kContentMargin;
    title_text_pos.y -= font.letter_height / 2;
    canvas.translate(title_text_pos.x, title_text_pos.y);
    font.DrawText(canvas, window_name, title_text_paint);
    canvas.restore();

    // Draw the locally cached image
    if (captured_image) {
      canvas.save();
      canvas.concat(layout.image_matrix);
      canvas.drawImage(captured_image, 0, 0, kFastSamplingOptions, nullptr);
      canvas.restore();
    }

    DrawChildren(canvas);
  }

  void FillChildren(Vec<Widget*>& children) override { children.push_back(pick_button.get()); }

  void ReleaseGrabs() {
    if (pointer_grab) pointer_grab->Release();
    if (key_grab) key_grab->Release();
  }
  void ReleaseGrab(ui::PointerGrab&) override { pointer_grab = nullptr; }
  void ReleaseKeyGrab(ui::KeyGrab&) override { key_grab = nullptr; }
  void KeyGrabberKeyDown(ui::KeyGrab&) override { ReleaseGrabs(); }
  void PointerGrabberButtonDown(ui::PointerGrab& grab, ui::PointerButton) override {
    ReleaseGrabs();
#ifdef __linux__

    xcb_window_t picked_window = XCB_WINDOW_NONE;
    {
      auto query_pointer_reply = xcb::query_pointer();
      picked_window = query_pointer_reply->child;
    }

    if (kDebugWindowPicking) {
      LOG << "Picked window: " << f("{:x}", picked_window);
      LOG_Indent();
    }

    // Find the first window that has WM_STATE
    xcb_window_t found_window = XCB_WINDOW_NONE;
    SearchWindows(picked_window, [&](xcb_window_t window, xcb_window_t parent) {
      if (kDebugWindowPicking) {
        LOG << "Checking for WM_STATE: " << f("{:x}", window);
      }
      if (HasWMState(window)) {
        if (kDebugWindowPicking) {
          LOG << "Found!";
        }
        found_window = window;
        return ControlFlow::StopSearching;
      }
      return ControlFlow::VisitChildren;
    });
    if (kDebugWindowPicking) {
      if (found_window != XCB_WINDOW_NONE) {
        auto name = xcb::GetPropertyString(found_window, XCB_ATOM_WM_NAME);
        LOG << "Found window: " << f("{:x}", found_window) << " " << name;
      } else {
        LOG << "No window found";
      }
      LOG_Unindent();
    }
    window_name = xcb::GetPropertyString(found_window, XCB_ATOM_WM_NAME);
    WakeAnimation();
    if (auto window = LockWindow()) {
      window->impl->xcb_window = found_window;
      window->title = window_name;
    }
#elif defined(_WIN32)
    POINT cursor_pos;
    if (!GetCursorPos(&cursor_pos)) {
      return;
    }

    HWND picked_window = WindowFromPoint(cursor_pos);
    if (kDebugWindowPicking) {
      LOG << "Picked window: " << f("{:x}", reinterpret_cast<uintptr_t>(picked_window));
    }

    // Find the top-level window
    HWND found_window = picked_window;
    while (found_window) {
      HWND parent = GetParent(found_window);
      if (!parent) break;
      found_window = parent;
    }

    // Ensure it's a valid window
    if (!IsValidWindow(found_window)) {
      if (kDebugWindowPicking) {
        LOG << "Invalid window selected";
      }
      return;
    }

    window_name = GetWindowTitle(found_window);
    if (kDebugWindowPicking) {
      LOG << "Found window: " << f("{:x}", reinterpret_cast<uintptr_t>(found_window)) << " "
          << window_name;
    }

    WakeAnimation();
    if (auto window = LockWindow()) {
      window->impl->hwnd = found_window;
      window->title = window_name;
    }
#endif
  }

  void VisitOptions(const OptionsVisitor& visitor) const override {
    WidgetBase::VisitOptions(visitor);
    if (auto window = LockWindow()) {
      auto lock = std::lock_guard(window->mutex);
      if (window->run_continuously) {
        DisableContinuousRunOption disable{window};
        visitor(disable);
      } else {
        EnableContinuousRunOption enable{window};
        visitor(enable);
      }
    }
  }
};

std::unique_ptr<Object::WidgetInterface> Window::MakeWidget(ui::Widget* parent) {
  return std::make_unique<WindowWidget>(parent, AcquireWeakPtr<Object>());
}

void Window::Args(std::function<void(Argument&)> cb) { cb(next_arg); }

void Window::OnRun(Location& here, RunTask&) {
  ZoneScopedN("Window");
#ifdef __linux__
  {
    auto lock = std::lock_guard(mutex);
    if (impl->xcb_window == XCB_WINDOW_NONE) {
      ReportError("No window selected");
      return;
      // TODO: if this HasError is used in more places, turn it into a helper function
    } else if (HasError(*this, [&](Error& err) {
                 if (err.reporter == this) {
                   err.Clear();
                 }
               })) {
      return;
    }

    // Initialize capture if not already done
    if (impl->data.empty()) {
      if (impl->shmseg == -1) {
        impl->shmseg = xcb_generate_id(xcb::connection);
      }
      int size = xcb::screen->width_in_pixels * xcb::screen->height_in_pixels * 4;
      if (impl->shmid == -1) {
        impl->shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777);
        xcb_shm_attach(xcb::connection, impl->shmseg, impl->shmid, false);
      }
      impl->data = std::span<char>(static_cast<char*>(shmat(impl->shmid, nullptr, 0)), size);
    }

    auto geometry_reply = xcb::get_geometry(impl->xcb_window);
    if (!geometry_reply) {
      return;
    }

    I16 x = 0;
    I16 y = 0;
    U16 width = geometry_reply->width;
    U16 height = geometry_reply->height;

    auto gtk_frame_extents_reply =
        xcb::get_property(impl->xcb_window, xcb::atom::_GTK_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 0, 4);
    if (gtk_frame_extents_reply->value_len == 4) {
      auto extents = (U32*)xcb_get_property_value(gtk_frame_extents_reply.get());
      x += extents[0];
      y += extents[2];
      width -= extents[0] + extents[1];
      height -= extents[2] + extents[3];
    }

    auto cookie = xcb_shm_get_image(xcb::connection, impl->xcb_window, x, y, width, height, ~0,
                                    XCB_IMAGE_FORMAT_Z_PIXMAP, impl->shmseg, 0);

    std::unique_ptr<xcb_shm_get_image_reply_t, xcb::FreeDeleter> reply(
        xcb_shm_get_image_reply(xcb::connection, cookie, nullptr));

    bool center_pixel_transparent = impl->data[(height / 2 * width + width / 2) * 4 + 3] == 0;

    int n = width * height;
    if (center_pixel_transparent) {
      for (int i = 0; i < n; ++i) {
        impl->data[i * 4 + 3] = 0xff;
      }
    }
    auto image_info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                        SkAlphaType::kUnpremul_SkAlphaType);
    auto pixmap = SkPixmap(image_info, impl->data.data(), width * 4);
    captured_image = SkImages::RasterFromPixmapCopy(pixmap);
    capture_time = time::SecondsSinceEpoch();
  }
#elif defined(_WIN32)
  {
    HWND hwnd;
    {
      auto lock = std::lock_guard(mutex);
      hwnd = impl->hwnd;
    }
    if (hwnd == nullptr) {
      ReportError("No window selected");
      return;
    }

    if (!IsWindow(hwnd)) {
      ReportError("Invalid window selected");
      return;
    }

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
      return;
    }

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (width <= 0 || height <= 0) {
      return;
    }

    HDC hdc_remote = GetDC(hwnd);
    HDC hdc_mem = CreateCompatibleDC(hdc_remote);

    if (!hdc_mem) {
      ReleaseDC(hwnd, hdc_remote);
      return;
    }

    // Get bitmap data
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;  // Negative for top-down DIB
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    void* bits;

    HBITMAP hbitmap =
        CreateDIBSection(hdc_mem, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbitmap) {
      DeleteDC(hdc_mem);
      ReleaseDC(hwnd, hdc_remote);
      return;
    }

    sk_sp<SkData> pixels = SkData::MakeWithProc(
        bits, width * height * 4, [](const void* bits, void* ctx) { DeleteObject((HBITMAP)ctx); },
        hbitmap);

    SelectObject(hdc_mem, hbitmap);

    // Try PrintWindow first (works better for some windows)
    BOOL printResult = PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT);
    if (!printResult) {
      // Fallback to BitBlt
      LOG << "PrintWindow failed, falling back to BitBlt";
      BitBlt(hdc_mem, 0, 0, width, height, hdc_remote, 0, 0, SRCCOPY);
    }

    DeleteDC(hdc_mem);
    ReleaseDC(hwnd, hdc_remote);

    auto image_info =
        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, SkAlphaType::kPremul_SkAlphaType);
    auto result = SkImages::RasterFromData(image_info, pixels, width * 4);
    {
      auto lock = std::lock_guard(mutex);
      captured_image = std::move(result);
      capture_time = time::SecondsSinceEpoch();
    }
  }
#endif
  WakeWidgetsAnimation();

  here.ScheduleUpdate();
  // Re-schedule execution if continuous run is enabled
  if (run_continuously) {
    here.ScheduleRun();
  }
}

void Window::Relocate(Location* new_here) {
  LiveObject::Relocate(new_here);
  if (run_continuously && new_here) {
    new_here->ScheduleRun();
  }
}

void Window::AttachToTitle() {
#ifdef __linux__
  SearchWindows(xcb::screen->root, [&](xcb_window_t window, xcb_window_t parent) {
    if (window == xcb::screen->root) {
      return ControlFlow::VisitChildren;
    }
    auto name = xcb::GetPropertyString(window, xcb::atom::WM_NAME);
    if (name != title) {
      return ControlFlow::SkipChildren;
    }
    if (HasWMState(window)) {
      impl->xcb_window = window;
      return ControlFlow::StopSearching;
    }
    return ControlFlow::VisitChildren;
  });
#elif defined(_WIN32)
  // Find window by title
  impl->hwnd = FindWindowA(nullptr, title.c_str());
  if (impl->hwnd && !IsValidWindow(impl->hwnd)) {
    impl->hwnd = nullptr;
  }
#endif
}

void Window::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();

  writer.Key("title");
  writer.String(title.data(), title.size());
  writer.Key("run_continuously");
  writer.Bool(run_continuously);
  writer.Key("capture_time");
  writer.Double(capture_time);

  writer.EndObject();
}

void Window::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto key : ObjectView(d, status)) {
    if (key == "title") {
      d.Get(title, status);
    } else if (key == "run_continuously") {
      d.Get(run_continuously, status);
    } else if (key == "capture_time") {
      d.Get(capture_time, status);
    }
    // Skip deprecated ratio fields for backward compatibility
  }
  if (!OK(status)) {
    ReportError(status.ToStr());
  }
  if (!title.empty()) {
    AttachToTitle();
  }
}

// ImageProvider interface implementation
sk_sp<SkImage> Window::GetImage() {
  auto lock = std::lock_guard(mutex);
  return captured_image;
}

ImageProvider* Window::AsImageProvider() { return this; }

}  // namespace automat::library
