// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#undef NOGDI
// clang-format off
#include <windows.h>
// clang-format on

#include <dwmapi.h>
#include <inspectable.h>
#include <objectarray.h>
#include <servprov.h>
#include <shobjidl.h>
#include <wrl/client.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#include <unordered_map>

#include "keyboard.hpp"
#include "log.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "vm.hpp"
#include "win32.hpp"
#include "win32_capture.hpp"
#include "win32_window.hpp"
#include "win32_window_manager.hpp"
#include "win_key.hpp"
#include "window_frame.hpp"

using namespace automat::library;

namespace automat::win32_wm {

using Microsoft::WRL::ComPtr;

constexpr auto kOffScreenPause = std::chrono::milliseconds(500);
constexpr auto kRepaintRetry = std::chrono::milliseconds(400);

// Everything below runs on the UI thread: the hook is installed there, so its
// events arrive through the same message loop that drives the board.
static std::unordered_map<HWND, WeakPtr<AppWindow>> tracked;

static std::mutex ui_mutex;
static ClientArrivals ui_arrivals;

static HWINEVENTHOOK object_hook = nullptr;
static HWINEVENTHOOK move_hook = nullptr;
static HWINEVENTHOOK cloak_hook = nullptr;

static Str WindowTitle(HWND hwnd) {
  int length = GetWindowTextLengthW(hwnd);
  if (length <= 0) return {};
  std::wstring wide((size_t)length + 1, L'\0');
  length = GetWindowTextW(hwnd, wide.data(), length + 1);
  wide.resize(std::max(0, length));
  return win32::WideToUtf8(wide);
}

static SkISize WindowSize(HWND hwnd) {
  RECT r = {};
  if (!GetWindowRect(hwnd, &r)) return {};
  return {r.right - r.left, r.bottom - r.top};
}

struct IApplicationViewChangeListener;
struct IApplicationViewOperation;
struct IApplicationViewPosition;
struct IAsyncCallback;
struct IImmersiveApplication;
struct IImmersiveMonitor;
using APPLICATION_VIEW_CLOAK_TYPE = UINT;
using APPLICATION_VIEW_COMPATIBILITY_POLICY = UINT;

MIDL_INTERFACE("372E1D3B-38D3-42E4-A15B-8AB2B178F513")
IApplicationView : public IInspectable {
  STDMETHOD(SetFocus)() = 0;
  STDMETHOD(SwitchTo)() = 0;
  STDMETHOD(TryInvokeBack)(IAsyncCallback*) = 0;
  STDMETHOD(GetThumbnailWindow)(HWND*) = 0;
  STDMETHOD(GetMonitor)(IImmersiveMonitor**) = 0;
  STDMETHOD(GetVisibility)(void*) = 0;
  STDMETHOD(SetCloak)(APPLICATION_VIEW_CLOAK_TYPE cloak_type, int flags) = 0;
  STDMETHOD(GetPosition)(const GUID*, void*) = 0;
  STDMETHOD(SetPosition)(IApplicationViewPosition*) = 0;
  STDMETHOD(InsertAfterWindow)(HWND) = 0;
  STDMETHOD(GetExtendedFramePosition)(RECT*) = 0;
  STDMETHOD(GetAppUserModelId)(PWSTR*) = 0;
  STDMETHOD(SetAppUserModelId)(PCWSTR) = 0;
  STDMETHOD(IsEqualByAppUserModelId)(PCWSTR, int*) = 0;
  STDMETHOD(GetViewState)(UINT*) = 0;
  STDMETHOD(SetViewState)(UINT) = 0;
  STDMETHOD(GetNeediness)(int*) = 0;
  STDMETHOD(GetLastActivationTimestamp)(ULONGLONG*) = 0;
  STDMETHOD(SetLastActivationTimestamp)(ULONGLONG) = 0;
  STDMETHOD(GetVirtualDesktopId)(GUID*) = 0;
  STDMETHOD(SetVirtualDesktopId)(const GUID*) = 0;
  STDMETHOD(GetShowInSwitchers)(int*) = 0;
  STDMETHOD(SetShowInSwitchers)(int) = 0;
  STDMETHOD(GetScaleFactor)(int*) = 0;
  STDMETHOD(CanReceiveInput)(BOOL*) = 0;
  STDMETHOD(GetCompatibilityPolicyType)(APPLICATION_VIEW_COMPATIBILITY_POLICY*) = 0;
  STDMETHOD(SetCompatibilityPolicyType)(APPLICATION_VIEW_COMPATIBILITY_POLICY) = 0;
  STDMETHOD(GetSizeConstraints)(IImmersiveMonitor*, SIZE*, SIZE*) = 0;
  STDMETHOD(GetSizeConstraintsForDpi)(UINT, SIZE*, SIZE*) = 0;
  STDMETHOD(SetSizeConstraintsForDpi)(const UINT*, const SIZE*, const SIZE*) = 0;
  STDMETHOD(OnMinSizePreferencesUpdated)(HWND) = 0;
  STDMETHOD(ApplyOperation)(IApplicationViewOperation*) = 0;
  STDMETHOD(IsTray)(BOOL*) = 0;
  STDMETHOD(IsInHighZOrderBand)(BOOL*) = 0;
  STDMETHOD(IsSplashScreenPresented)(BOOL*) = 0;
  STDMETHOD(Flash)() = 0;
  STDMETHOD(GetRootSwitchableOwner)(IApplicationView**) = 0;
  STDMETHOD(EnumerateOwnershipTree)(IObjectArray**) = 0;
  STDMETHOD(GetEnterpriseId)(PWSTR*) = 0;
  STDMETHOD(IsMirrored)(BOOL*) = 0;
  STDMETHOD(Unknown1)(int*) = 0;
  STDMETHOD(Unknown2)(int*) = 0;
  STDMETHOD(Unknown3)(int*) = 0;
  STDMETHOD(Unknown4)(int) = 0;
  STDMETHOD(Unknown5)(int*) = 0;
  STDMETHOD(Unknown6)(int) = 0;
  STDMETHOD(Unknown7)() = 0;
  STDMETHOD(Unknown8)(int*) = 0;
  STDMETHOD(Unknown9)(int) = 0;
  STDMETHOD(Unknown10)(int, int) = 0;
  STDMETHOD(Unknown11)(int) = 0;
  STDMETHOD(Unknown12)(SIZE*) = 0;
};

MIDL_INTERFACE("1841C6D7-4F9D-42C0-AF41-8747538F10E5")
IApplicationViewCollection : public IUnknown {
  STDMETHOD(GetViews)(IObjectArray**) = 0;
  STDMETHOD(GetViewsByZOrder)(IObjectArray**) = 0;
  STDMETHOD(GetViewsByAppUserModelId)(PCWSTR, IObjectArray**) = 0;
  STDMETHOD(GetViewForHwnd)(HWND, IApplicationView**) = 0;
  STDMETHOD(GetViewForApplication)(IImmersiveApplication*, IApplicationView**) = 0;
  STDMETHOD(GetViewForAppUserModelId)(PCWSTR, IApplicationView**) = 0;
  STDMETHOD(GetViewInFocus)(IApplicationView**) = 0;
  STDMETHOD(TryGetLastActiveVisibleView)(IApplicationView**) = 0;
  STDMETHOD(RefreshCollection)() = 0;
  STDMETHOD(RegisterForApplicationViewChanges)(IApplicationViewChangeListener*, DWORD*) = 0;
  STDMETHOD(UnregisterForApplicationViewChanges)(DWORD) = 0;
};

constexpr CLSID kImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};

static HRESULT AppViewFor(HWND hwnd, ComPtr<IApplicationView>& view) {
  ComPtr<IServiceProvider> shell;
  HRESULT hr = CoCreateInstance(kImmersiveShell, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shell));
  if (FAILED(hr)) return hr;
  ComPtr<IApplicationViewCollection> views;
  hr = shell->QueryService(__uuidof(IApplicationViewCollection), IID_PPV_ARGS(&views));
  if (FAILED(hr)) return hr;
  return views->GetViewForHwnd(hwnd, &view);
}

static HRESULT CloakThroughShell(HWND hwnd, int flags) {
  ComPtr<IApplicationView> view;
  HRESULT hr = AppViewFor(hwnd, view);
  if (FAILED(hr)) return hr;
  return view->SetCloak(1, flags);
}

static bool ShellCloaked(HWND hwnd) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
         cloaked != 0;
}

static void SetCloaked(HWND hwnd, bool cloaked) {
  if (!IsWindow(hwnd)) return;
  if (ShellCloaked(hwnd) == cloaked) return;
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HRESULT hr = CloakThroughShell(hwnd, cloaked ? 2 : 0);
  if (SUCCEEDED(init)) CoUninitialize();
  if (FAILED(hr)) {
    ERROR << "Could not " << (cloaked ? "cloak " : "uncloak ") << WindowTitle(hwnd) << ": "
          << win32::ErrorStr(hr);
  }
}

static HWND hidden_owner = nullptr;

static void SetHiddenFromSwitchers(AppWindow& window, HWND hwnd, bool hidden) {
  if (!IsWindow(hwnd)) return;
  if (hidden) {
    window.prev_owner = (void*)GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT);
    if (hidden_owner) SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hidden_owner);
  } else {
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)window.prev_owner);
    window.prev_owner = nullptr;
  }
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<ITaskbarList> taskbar;
  if (SUCCEEDED(
          CoCreateInstance(__uuidof(TaskbarList), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&taskbar))) &&
      SUCCEEDED(taskbar->HrInit())) {
    if (hidden) {
      taskbar->DeleteTab(hwnd);
    } else {
      taskbar->AddTab(hwnd);
    }
  }
  if (SUCCEEDED(init)) CoUninitialize();
}

// The compositor only produces a new frame when the program paints, so a
// window that has not painted since it was embedded is asked to.
static void RequestRepaint(HWND hwnd) {
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static bool Transient(HWND hwnd) {
  if (GetWindowLongW(hwnd, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return true;
  wchar_t class_name[16] = {};
  if (GetClassNameW(hwnd, class_name, 16) && wcscmp(class_name, L"#32768") == 0) return true;
  return false;
}

// The windows a user would see in the taskbar: on screen and not a helper
// window.
static bool CanEmbed(HWND hwnd) {
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
  if (Transient(hwnd)) return false;
  SkISize size = WindowSize(hwnd);
  return size.width() > 1 && size.height() > 1;
}

static Ptr<AppWindow> Find(HWND hwnd) {
  auto it = tracked.find(hwnd);
  if (it == tracked.end()) return nullptr;
  auto window = it->second.Lock();
  if (!window) tracked.erase(it);
  return window;
}

static std::unordered_map<HWND, WeakPtr<AppWindow>> popup_index;

static void MirrorPopup(HWND hwnd) {
  if (popup_index.contains(hwnd) || tracked.contains(hwnd)) return;
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return;
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
  if (!Transient(hwnd)) return;
  Ptr<AppWindow> window;
  for (HWND owner = GetWindow(hwnd, GW_OWNER); owner && !window;
       owner = GetWindow(owner, GW_OWNER)) {
    window = Find(owner);
  }
  if (!window) {
    DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    for (auto& [tracked_hwnd, weak] : tracked) {
      if (GetWindowThreadProcessId(tracked_hwnd, nullptr) != thread) continue;
      window = weak.Lock();
      if (window) break;
    }
  }
  if (!window) return;
  RECT rect = {};
  if (!GetWindowRect(hwnd, &rect)) return;
  Status status;
  auto capture = StartCapture(
      hwnd,
      [weak = window->AcquireWeakPtr(), hwnd](sk_sp<SkImage> image, SkISize size) {
        auto win = weak.Lock();
        if (!win) return;
        {
          auto lock = std::lock_guard(win->mutex);
          for (auto& popup : win->popups) {
            if (popup.hwnd == hwnd) {
              popup.image = std::move(image);
              popup.size = size;
            }
          }
        }
        win->WakeToys();
        vm.WakeToys();
      },
      status);
  if (!capture) return;
  {
    auto lock = std::lock_guard(window->mutex);
    window->popups.push_back({.hwnd = hwnd,
                              .capture = std::move(capture),
                              .screen_pos = SkIPoint::Make(rect.left, rect.top)});
  }
  popup_index[hwnd] = window->AcquireWeakPtr();
  window->WakeToys();
  vm.WakeToys();
}

static void PopupMoved(HWND hwnd) {
  auto it = popup_index.find(hwnd);
  if (it == popup_index.end()) return;
  auto window = it->second.Lock();
  if (!window) {
    popup_index.erase(it);
    return;
  }
  RECT rect = {};
  if (!GetWindowRect(hwnd, &rect)) return;
  {
    auto lock = std::lock_guard(window->mutex);
    for (auto& popup : window->popups) {
      if (popup.hwnd == hwnd) popup.screen_pos = SkIPoint::Make(rect.left, rect.top);
    }
  }
  window->WakeToys();
  vm.WakeToys();
}

static void DropPopup(HWND hwnd) {
  auto it = popup_index.find(hwnd);
  if (it == popup_index.end()) return;
  auto window = it->second.Lock();
  popup_index.erase(it);
  if (!window) return;
  std::unique_ptr<Capture> capture;
  {
    auto lock = std::lock_guard(window->mutex);
    for (size_t i = 0; i < window->popups.size(); ++i) {
      if (window->popups[i].hwnd == hwnd) {
        capture = std::move(window->popups[i].capture);
        window->popups.erase(window->popups.begin() + i);
        break;
      }
    }
  }
  window->WakeToys();
  vm.WakeToys();
}

static void StartCaptureFor(AppWindow& window) {
  Status status;
  auto capture = StartCapture(
      window.hwnd.load(),
      [weak = window.AcquireWeakPtr()](sk_sp<SkImage> image, SkISize size) {
        auto win = weak.Lock();
        if (!win) return;
        {
          auto lock = std::lock_guard(win->mutex);
          win->image = std::move(image);
          win->content_size = size;
        }
        win->WakeToys();
        vm.WakeToys();
      },
      status);
  if (!capture) {
    ERROR << "Could not capture " << window.title << ": " << status.ToStr();
    return;
  }
  window.capture = std::move(capture);
}

static void ApplyMode(AppWindow& window) {
  HWND hwnd = window.hwnd.load();
  if (hwnd == nullptr) return;
  if (window.mode.load(std::memory_order_relaxed) == AppWindow::Mode::Embedded) {
    SetHiddenFromSwitchers(window, hwnd, true);
    SetCloaked(hwnd, true);
  } else {
    SetHiddenFromSwitchers(window, hwnd, false);
    SetCloaked(hwnd, false);
  }
  window.WakeToys();
}

static void Adopt(HWND hwnd) {
  if (tracked.contains(hwnd)) return;
  if (!CanEmbed(hwnd)) return;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0) return;
  Ptr<Launch> launch = Launch::Find(pid);
  if (!launch) return;
  if (ShellCloaked(hwnd)) {
    SetCloaked(hwnd, false);
    if (ShellCloaked(hwnd)) return;
  }

  auto window = launch->LockRestoring<AppWindow>();
  bool restored = (bool)window;
  if (!window) window = MAKE_PTR(AppWindow);
  window->hwnd.store(hwnd);
  tracked[hwnd] = window->AcquireWeakPtr();
  {
    auto lock = std::lock_guard(window->mutex);
    window->client_gone = false;
    window->client_pid = pid;
    window->title = WindowTitle(hwnd);
    window->client_decorated = (GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) != 0;
    window->content_size = WindowSize(hwnd);
    if (!restored) {
      window->recipe = launch->argv;
      window->launched_by = launch;
    }
  }
  StartCaptureFor(*window);
  ApplyMode(*window);
  RequestRepaint(hwnd);
  launch->WindowAppeared();
  if (restored) {
    launch->RestoredInto(*window);
  } else {
    auto lock = std::lock_guard(ui_mutex);
    ui_arrivals.appeared.emplace_back(window, launch);
  }
  window->WakeToys();
  vm.WakeToys();
}

static void Drop(HWND hwnd) {
  auto window = Find(hwnd);
  tracked.erase(hwnd);
  if (!window) return;
  if (window->mode.load(std::memory_order_relaxed) == AppWindow::Mode::Embedded) {
    SetCloaked(hwnd, false);
    SetHiddenFromSwitchers(*window, hwnd, false);
  }
  window->hwnd.store(nullptr);
  window->capture.reset();
  {
    auto lock = std::lock_guard(window->mutex);
    window->client_gone = true;
  }
  window->WakeToys();
  {
    auto lock = std::lock_guard(ui_mutex);
    ui_arrivals.disappeared.push_back(window);
  }
  vm.WakeToys();
}

static void TitleChanged(HWND hwnd) {
  auto window = Find(hwnd);
  if (!window) return;
  Str title = WindowTitle(hwnd);
  {
    auto lock = std::lock_guard(window->mutex);
    if (window->title == title) return;
    window->title = std::move(title);
  }
  window->WakeToys();
}

static void CALLBACK OnWindowEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object_id,
                                   LONG child_id, DWORD, DWORD) {
  if (hwnd == nullptr || object_id != OBJID_WINDOW || child_id != CHILDID_SELF) return;
  switch (event) {
    case EVENT_OBJECT_SHOW:
      Adopt(hwnd);
      MirrorPopup(hwnd);
      break;
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_DESTROY:
      Drop(hwnd);
      DropPopup(hwnd);
      break;
    case EVENT_OBJECT_LOCATIONCHANGE:
      PopupMoved(hwnd);
      break;
    case EVENT_OBJECT_NAMECHANGE:
      TitleChanged(hwnd);
      break;
    case EVENT_OBJECT_UNCLOAKED:
      if (auto window = Find(hwnd)) {
        if (window->mode.load(std::memory_order_relaxed) == AppWindow::Mode::Embedded) {
          auto now = time::SteadyNow();
          if (now - window->last_recloak > std::chrono::seconds(1)) {
            window->last_recloak = now;
            SetCloaked(hwnd, true);
          }
        }
      }
      break;
    case EVENT_SYSTEM_MOVESIZESTART:
      if (auto window = Find(hwnd)) {
        if (window->input_action) {
          PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
          auto lock = std::lock_guard(ui_mutex);
          ui_arrivals.move_requests.push_back(window->AcquireWeakPtr());
        }
      }
      break;
  }
}

bool CloseWindowsOf(Launch& launch, bool keep_connected) {
  bool any = false;
  for (auto& [hwnd, weak] : tracked) {
    auto window = weak.Lock();
    if (!window) continue;
    if (window->launched_by.Get() != &launch) continue;
    any = true;
    if (keep_connected &&
        window->mode.load(std::memory_order_relaxed) == AppWindow::Mode::Connected) {
      continue;
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
  return any;
}

// ============================================================================
//  The board object
// ============================================================================

struct PopupDraw {
  sk_sp<SkImage> image;
  SkIRect rect_px;
};

static POINT MappingOrigin(HWND hwnd, SkISize content) {
  POINT origin = {};
  RECT window_rect = {};
  if (hwnd && GetWindowRect(hwnd, &window_rect)) {
    RECT extended = {};
    bool window_matches = window_rect.right - window_rect.left == content.width() &&
                          window_rect.bottom - window_rect.top == content.height();
    if (!window_matches &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extended,
                                        sizeof(extended))) &&
        extended.right - extended.left == content.width() &&
        extended.bottom - extended.top == content.height()) {
      origin = {extended.left, extended.top};
    } else {
      origin = {window_rect.left, window_rect.top};
    }
  }
  return origin;
}

struct AppWindowToy : ClientWindowToy {
  AppWindow::Mode mode_ = AppWindow::Mode::Embedded;
  mutable time::SteadyPoint last_drawn_ = time::kZeroSteady;
  mutable bool capture_paused_ = false;
  time::SteadyPoint last_repaint_asked_ = time::kZeroSteady;
  WPARAM buttons_down_ = 0;
  HWND grab_ = nullptr;
  mutable HWND last_mouse_target_ = nullptr;
  Vec<PopupDraw> popups_;
  uint64_t popups_key_ = 0;
  bool popups_changed_ = false;

  AppWindowToy(ui::Widget* parent, Object& obj) : ClientWindowToy(parent, obj) {
    last_drawn_ = time::SteadyNow();
    PullState();
  }

  Ptr<AppWindow> LockWindow() const { return LockObject<AppWindow>(); }

  void PullMore(ClientWindow& window) override {
    auto& win = static_cast<AppWindow&>(window);
    mode_ = win.mode.load(std::memory_order_relaxed);
    popups_.clear();
    uint64_t key = 0;
    if (!win.popups.empty()) {
      POINT origin = MappingOrigin((HWND)win.hwnd.load(), content_size_);
      for (auto& popup : win.popups) {
        if (!popup.image) continue;
        SkIRect rect =
            SkIRect::MakeXYWH(popup.screen_pos.x() - origin.x, popup.screen_pos.y() - origin.y,
                              popup.size.width(), popup.size.height());
        key = key * 1000003 + (uint64_t)(uint32_t)rect.x() * 31 + (uint64_t)(uint32_t)rect.y() * 7 +
              (uint64_t)(uint32_t)rect.width() * 3 + (uint64_t)(uint32_t)rect.height() + 1;
        popups_.push_back({popup.image, rect});
      }
    }
    popups_changed_ = key != popups_key_;
    popups_key_ = key;
  }

  void PauseCapture(bool paused) const {
    if (capture_paused_ == paused) return;
    capture_paused_ = paused;
    if (auto win = LockWindow()) {
      if (win->capture) win->capture->Pause(paused);
      if (!paused) RequestRepaint(win->hwnd.load());
    }
  }

  bool TickMore(time::Timer& timer) override {
    PauseCapture(timer.now - last_drawn_ > kOffScreenPause);
    if (auto win = LockWindow()) {
      HWND hwnd = win->hwnd.load();
      if (hwnd) {
        if (mode_ == AppWindow::Mode::Embedded && IsIconic(hwnd)) {
          ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        if (!image_ && !capture_paused_ && timer.now - last_repaint_asked_ > kRepaintRetry) {
          last_repaint_asked_ = timer.now;
          RequestRepaint(hwnd);
        }
      }
    }
    return std::exchange(popups_changed_, false);
  }

  Rect PopupToyRect(const PopupDraw& popup) const {
    Vec2 sz = ContentSize();
    float left = popup.rect_px.x() * kPx - sz.x / 2;
    float top = sz.y / 2 - popup.rect_px.y() * kPx;
    return Rect{left, top - popup.rect_px.height() * kPx, left + popup.rect_px.width() * kPx, top};
  }

  Optional<Rect> DrawBounds() const override {
    SkRect bounds = Shape().getBounds();
    for (auto& popup : popups_) bounds.join(PopupToyRect(popup).sk);
    return bounds;
  }

  void Draw(SkCanvas& canvas) const override {
    last_drawn_ = time::SteadyNow();
    PauseCapture(false);
    ClientWindowToy::Draw(canvas);
    for (auto& popup : popups_) {
      Rect dst = PopupToyRect(popup);
      canvas.save();
      canvas.translate(dst.left, dst.top);
      canvas.scale(1, -1);
      canvas.drawImageRect(popup.image, SkRect::MakeWH(dst.Width(), dst.Height()),
                           SkSamplingOptions(SkFilterMode::kLinear));
      canvas.restore();
    }
  }

  // ---- input pass-through ----

  POINT SurfaceToScreen(Vec2 px) const {
    POINT result = {(LONG)lroundf(px.x), (LONG)lroundf(px.y)};
    if (auto win = LockWindow()) {
      POINT origin = MappingOrigin((HWND)win->hwnd.load(), content_size_);
      result.x += origin.x;
      result.y += origin.y;
    }
    return result;
  }

  POINT ScreenAt(ui::Pointer& p) const {
    return SurfaceToScreen(ToSurfacePx(p.PositionWithin(*this)));
  }

  HWND DeepestChildAt(POINT screen) const {
    auto win = LockWindow();
    if (!win) return nullptr;
    HWND target = win->hwnd.load();
    if (target == nullptr) return nullptr;
    for (int depth = 0; depth < 8; ++depth) {
      POINT local = screen;
      ScreenToClient(target, &local);
      HWND child = RealChildWindowFromPoint(target, local);
      if (child == nullptr || child == target) break;
      target = child;
    }
    return target;
  }

  void PostMouse(UINT message, WPARAM wparam, POINT screen) const {
    HWND target = grab_ ? grab_ : DeepestChildAt(screen);
    if (target == nullptr) return;
    last_mouse_target_ = target;
    POINT client = screen;
    ScreenToClient(target, &client);
    PostMessageW(target, message, wparam, MAKELPARAM((short)client.x, (short)client.y));
  }

  LRESULT HitTest(POINT screen) const {
    auto win = LockWindow();
    if (!win) return HTCLIENT;
    HWND hwnd = win->hwnd.load();
    if (hwnd == nullptr) return HTCLIENT;
    DWORD_PTR result = HTCLIENT;
    if (!SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM((short)screen.x, (short)screen.y),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result)) {
      return HTCLIENT;
    }
    return (LRESULT)result;
  }

  void SendMotion(Vec2 px) override { PostMouse(WM_MOUSEMOVE, buttons_down_, SurfaceToScreen(px)); }

  void SendCrossing(bool enter, Vec2) override {
    if (enter) return;
    if (last_mouse_target_ != nullptr) {
      PostMessageW(last_mouse_target_, WM_MOUSELEAVE, 0, 0);
      last_mouse_target_ = nullptr;
    }
  }

  void SendFocus(bool in) override {
    auto win = LockWindow();
    if (!win) return;
    HWND hwnd = win->hwnd.load();
    if (hwnd == nullptr) return;
    PostMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(in ? WA_ACTIVE : WA_INACTIVE, 0), 0);
  }

  HWND KeyboardTarget() const {
    auto win = LockWindow();
    if (!win) return nullptr;
    HWND hwnd = win->hwnd.load();
    if (hwnd == nullptr) return nullptr;
    GUITHREADINFO info = {.cbSize = sizeof(GUITHREADINFO)};
    DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    if (GetGUIThreadInfo(thread, &info) && info.hwndFocus) return info.hwndFocus;
    return hwnd;
  }

  void SendKey(ui::Key key, bool pressed) override {
    HWND target = KeyboardTarget();
    if (target == nullptr) return;
    UINT virtual_key = KeyToVirtualKey(key.physical);
    if (virtual_key == 0) return;
    UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
    LPARAM lparam = 1 | ((LPARAM)(scan_code & 0xFF) << 16);
    if ((scan_code >> 8) == 0xE0) lparam |= (LPARAM)1 << 24;
    if (pressed) {
      PostMessageW(target, WM_KEYDOWN, virtual_key, lparam);
    } else {
      PostMessageW(target, WM_KEYUP, virtual_key, lparam | (LPARAM)0xC0000000);
    }
  }

  bool AllowClientPress(ui::Pointer& p) override;
  std::unique_ptr<Action> BeginClientPress(ui::Pointer& p) override;

  void VisitOptions(const OptionsVisitor& visitor) const override;
};

// Held while a button pressed over the window is down: routes the press and
// the release to the program instead of dragging the board object.
struct AppInputAction : ClientInputActionBase {
  AppWindowToy& toy;
  UINT down_message;
  UINT up_message;
  WPARAM button_flag;

  AppInputAction(ui::Pointer& p, AppWindowToy& toy, UINT down_message, UINT up_message,
                 WPARAM button_flag)
      : ClientInputActionBase(p),
        toy(toy),
        down_message(down_message),
        up_message(up_message),
        button_flag(button_flag) {
    toy.FocusClient(p);
    if (auto win = toy.LockWindow()) LinkWindow(*win);
    toy.grab_ = toy.DeepestChildAt(toy.ScreenAt(p));
    toy.buttons_down_ |= button_flag;
    toy.PostMouse(down_message, toy.buttons_down_, toy.ScreenAt(p));
  }
  void Update() override {}
  ui::Widget& InitiatingWidget() override { return toy; }
  ~AppInputAction() override {
    toy.buttons_down_ &= ~button_flag;
    toy.PostMouse(up_message, toy.buttons_down_, toy.ScreenAt(pointer));
    toy.grab_ = nullptr;
  }
};

bool AppWindowToy::AllowClientPress(ui::Pointer& p) {
  LRESULT where = HitTest(ScreenAt(p));
  bool on_frame = where == HTCAPTION || where == HTSYSMENU || where == HTMINBUTTON ||
                  where == HTMAXBUTTON || where == HTZOOM;
  return !on_frame;
}

std::unique_ptr<Action> AppWindowToy::BeginClientPress(ui::Pointer& p) {
  return std::make_unique<AppInputAction>(p, *this, WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON);
}

static void PostApplyMode(const WeakPtr<AppWindow>& weak) {
  auto* main_window = dynamic_cast<Win32Window*>(ui::root_widget->window.get());
  if (main_window == nullptr || main_window->hwnd == nullptr) return;
  auto* apply = new std::function<void()>([weak = weak.Copy()] {
    auto window = weak.Lock();
    if (!window) return;
    ApplyMode(*window);
    if (window->mode.load(std::memory_order_relaxed) == AppWindow::Mode::Connected) {
      if (HWND hwnd = window->hwnd.load()) {
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(hwnd);
      }
    }
  });
  PostMessageW(main_window->hwnd, WM_USER, 0, (LPARAM)apply);
}

struct ModeOption : TextOption {
  WeakPtr<AppWindow> window;
  AppWindow::Mode target;
  ModeOption(WeakPtr<AppWindow>&& window, AppWindow::Mode target)
      : TextOption(target == AppWindow::Mode::Connected ? "Pop Out" : "Embed"),
        window(std::move(window)),
        target(target) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<ModeOption>(window.Copy(), target);
  }
  std::unique_ptr<Action> Activate(ui::Pointer&) const override {
    if (auto w = window.Lock()) {
      w->mode.store(target, std::memory_order_relaxed);
      PostApplyMode(w->AcquireWeakPtr());
    }
    return nullptr;
  }
  Option::Dir PreferredDir() const override { return Option::S; }
};

void AppWindowToy::VisitOptions(const OptionsVisitor& visitor) const {
  ClientWindowToy::VisitOptions(visitor);
  ModeOption toggle(owner.Copy<AppWindow>(), mode_ == AppWindow::Mode::Embedded
                                                 ? AppWindow::Mode::Connected
                                                 : AppWindow::Mode::Embedded);
  visitor(toggle);
}

// ============================================================================
//  Module lifecycle
// ============================================================================

void Start(Status& status) {
  hidden_owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
  object_hook =
      SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_NAMECHANGE, nullptr, OnWindowEvent, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  move_hook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
                              OnWindowEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  cloak_hook = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnWindowEvent,
                               0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  if (object_hook == nullptr || move_hook == nullptr || cloak_hook == nullptr) {
    AppendErrorMessage(status) += "Could not watch for new windows.";
  }
}

void Stop() {
  if (object_hook) UnhookWinEvent(object_hook);
  if (move_hook) UnhookWinEvent(move_hook);
  if (cloak_hook) UnhookWinEvent(cloak_hook);
  object_hook = nullptr;
  move_hook = nullptr;
  cloak_hook = nullptr;
  for (auto& [hwnd, weak] : tracked) {
    auto window = weak.Lock();
    if (!window) continue;
    window->capture.reset();
    window->hwnd.store(nullptr);
    Vec<AppWindow::PopupMirror> popups;
    {
      auto lock = std::lock_guard(window->mutex);
      popups.swap(window->popups);
    }
    popups.clear();
    if (window->mode.load(std::memory_order_relaxed) == AppWindow::Mode::Embedded) {
      SetCloaked(hwnd, false);
      SetHiddenFromSwitchers(*window, hwnd, false);
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }
  tracked.clear();
  popup_index.clear();
  if (hidden_owner) {
    DestroyWindow(hidden_owner);
    hidden_owner = nullptr;
  }
  {
    auto lock = std::lock_guard(ui_mutex);
    ui_arrivals.appeared.clear();
    ui_arrivals.disappeared.clear();
    ui_arrivals.move_requests.clear();
  }
  ReleaseCaptureDevice();
}

void Tick() {
  if (object_hook == nullptr) return;
  ClientArrivals arrivals;
  {
    auto lock = std::lock_guard(ui_mutex);
    arrivals.appeared.swap(ui_arrivals.appeared);
    arrivals.disappeared.swap(ui_arrivals.disappeared);
    arrivals.move_requests.swap(ui_arrivals.move_requests);
  }
  arrivals.Process();
}

}  // namespace automat::win32_wm

namespace automat::library {

AppWindow::AppWindow() = default;

AppWindow::AppWindow(const AppWindow& o) : ClientWindow(o) {
  mode.store(o.mode.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

AppWindow::~AppWindow() {
  capture.reset();
  if (HWND hwnd = this->hwnd.load()) {
    if (mode.load(std::memory_order_relaxed) == Mode::Embedded) {
      win32_wm::SetCloaked(hwnd, false);
      win32_wm::SetHiddenFromSwitchers(*this, hwnd, false);
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }
}

void AppWindow::DecorationPreferenceChanged() { WakeToys(); }

Ptr<Object> AppWindow::Clone() const {
  auto window = MAKE_PTR(AppWindow, *this);
  LaunchClone(*this, *window);
  return window;
}

std::unique_ptr<ObjectToy> AppWindow::MakeToy(ui::Widget* parent) {
  return std::make_unique<win32_wm::AppWindowToy>(parent, *this);
}

void AppWindow::SerializeState(ObjectSerializer& writer) const {
  ClientWindow::SerializeState(writer);
  if (mode.load(std::memory_order_relaxed) == Mode::Connected) {
    StrView v = "connected";
    writer.Key("mode");
    writer.String(v.data(), v.size());
  }
}

bool AppWindow::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "mode") {
    Status status;
    Str v;
    d.Get(v, status);
    mode.store(v == "connected" ? Mode::Connected : Mode::Embedded, std::memory_order_relaxed);
    return true;
  }
  return ClientWindow::DeserializeKey(d, key);
}

}  // namespace automat::library
