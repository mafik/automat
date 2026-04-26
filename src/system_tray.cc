// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#ifdef _WIN32
#include "win32.hh"
#include "win32_window.hh"
#pragma push_macro("ERROR")
#include <shellapi.h>
#include <shlobj_core.h>
#pragma pop_macro("ERROR")
#pragma comment(lib, "shell32.lib")
#endif  // _WIN32

#include "root_widget.hh"
#ifdef __linux__
#include <sdbus-c++/sdbus-c++.h>

#include "../build/generated/com.canonical.dbusmenu_adaptor.hh"
#include "../build/generated/org.kde.StatusNotifierItem_adaptor.hh"
#include "../build/generated/org.kde.StatusNotifierWatcher_proxy.hh"
#endif  // __linux__

#include <include/core/SkAlphaType.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkColorType.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>

#include <bit>
#include <cstddef>
#include <map>

#include "automat.hh"
#include "format.hh"
#include "id_pool.hh"
#include "log.hh"
#include "system_tray.hh"

namespace automat {

namespace system_tray {
static_assert(offsetof(Spacer, item) == 0);
static_assert(offsetof(Action, item) == 0);
static_assert(offsetof(Menu, item) == 0);
static_assert(offsetof(SkiaIcon, icon) == 0);
static_assert(offsetof(WindowsStockIcon, icon) == 0);
static_assert(offsetof(FreedesktopIcon, icon) == 0);
}  // namespace system_tray

namespace {

IdPool& IconUidPool() {
  static IdPool pool(1, 0x10000);
  return pool;
}

system_tray::MenuIcon* NextInChain(system_tray::MenuIcon* icon) {
  using namespace system_tray;
  if (!icon) return nullptr;
  switch (icon->kind) {
    case MenuIcon::SkiaIconKind:
      return reinterpret_cast<SkiaIcon*>(icon)->fallback;
    case MenuIcon::WindowsStockIconKind:
      return reinterpret_cast<WindowsStockIcon*>(icon)->fallback;
    case MenuIcon::FreedesktopIconKind:
      return reinterpret_cast<FreedesktopIcon*>(icon)->fallback;
  }
  return nullptr;
}

}  // namespace

#ifdef __linux__

using std::map;
using std::string;
using std::tuple;
using std::vector;
using namespace sdbus;

namespace {

// Single-link try-helpers. Each returns an empty result if this link's kind isn't usable in
// this context (or if the load fails). Consumer walks the chain via NextInChain until one
// succeeds.
string TryGetIconName(system_tray::MenuIcon* link) {
  using namespace system_tray;
  if (!link || link->kind != MenuIcon::FreedesktopIconKind) return {};
  auto* f = reinterpret_cast<FreedesktopIcon*>(link);
  return f->name;
}

sk_sp<SkImage> TryGetIconImage(system_tray::MenuIcon* link) {
  using namespace system_tray;
  if (!link || link->kind != MenuIcon::SkiaIconKind) return {};
  auto* s = reinterpret_cast<SkiaIcon*>(link);
  return s->image;
}

void AppendSNIPixmaps(const sk_sp<SkImage>& image,
                      vector<Struct<int32_t, int32_t, vector<uint8_t>>>& pixmaps) {
  auto sampling_options = SkSamplingOptions(SkCubicResampler::CatmullRom());
  for (int size : {16, 22, 24, 32, 48, 64}) {
    auto& dbus_pixmap = pixmaps.emplace_back();
    dbus_pixmap.get<0>() = size;
    dbus_pixmap.get<1>() = size;
    dbus_pixmap.get<2>() = vector<uint8_t>(size * size * 4);
    auto& vec = dbus_pixmap.get<2>();
    auto image_info =
        SkImageInfo::Make(size, size, SkColorType::kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkPixmap sk_pixmap(image_info, vec.data(), size * 4);
    image->scalePixels(sk_pixmap, sampling_options);
    for (int i = 0; i < size * size; ++i) {
      U32* pixel = (U32*)(vec.data() + i * 4);
      *pixel = std::rotl(*pixel, 8);
    }
  }
}

}  // namespace

const ObjectPath kStatusNotifierPath{"/StatusNotifierItem"};
const ObjectPath kMenuPath{"/MenuBar"};

class StatusNotifierWatcherProxy final
    : public ProxyInterfaces<org::kde::StatusNotifierWatcher_proxy> {
 public:
  StatusNotifierWatcherProxy(IConnection& connection)
      : ProxyInterfaces(connection, ServiceName{"org.kde.StatusNotifierWatcher"},
                        ObjectPath{"/StatusNotifierWatcher"}) {
    registerProxy();
  }
  ~StatusNotifierWatcherProxy() { unregisterProxy(); }

 protected:
  void onStatusNotifierItemRegistered(const string& item) override {}
  void onStatusNotifierItemUnregistered(const string& item) override {}
  void onStatusNotifierHostRegistered() override {}
};

class StatusNotifierItem final : public AdaptorInterfaces<org::kde::StatusNotifierItem_adaptor> {
  using IconPixmap_t = Struct<int32_t, int32_t, vector<uint8_t>>;
  using IconPixmapList = vector<IconPixmap_t>;

  string title;
  string icon_name;
  IconPixmapList icon_pixmaps;
  Fn<void()> on_activate;

 public:
  StatusNotifierItem(IConnection& connection, string title, system_tray::MenuIcon* icon_chain,
                     Fn<void()> on_activate)
      : AdaptorInterfaces(connection, kStatusNotifierPath),
        title(std::move(title)),
        on_activate(std::move(on_activate)) {
    for (auto* link = icon_chain; link; link = NextInChain(link)) {
      if (string name = TryGetIconName(link); !name.empty()) {
        icon_name = std::move(name);
        break;
      }
      if (auto image = TryGetIconImage(link)) {
        AppendSNIPixmaps(image, icon_pixmaps);
        break;
      }
    }
    registerAdaptor();
  }
  ~StatusNotifierItem() { unregisterAdaptor(); }

 protected:
  string Category() override { return "ApplicationStatus"; }
  string Id() override { return "automat"; }
  string Title() override { return title; }
  string Status() override { return "Active"; }
  uint32_t WindowId() override { return 0; }
  string IconName() override { return icon_name; }
  IconPixmapList IconPixmap() override { return icon_pixmaps; }
  string OverlayIconName() override { return ""; }
  IconPixmapList OverlayIconPixmap() override { return {}; }
  string AttentionIconName() override { return ""; }
  IconPixmapList AttentionIconPixmap() override { return {}; }
  string AttentionMovieName() override { return ""; }
  Struct<string, IconPixmapList, string, string> ToolTip() override { return {"", {}, title, ""}; }
  bool ItemIsMenu() override { return false; }
  ObjectPath Menu() override { return kMenuPath; }

  void ContextMenu(const int32_t& x, const int32_t& y) override {}
  void Activate(const int32_t& x, const int32_t& y) override {
    if (on_activate) on_activate();
  }
  void SecondaryActivate(const int32_t& x, const int32_t& y) override {}
  void Scroll(const int32_t& delta, const string& orientation) override {}
};

struct DBusMenuItem {
  string type{"standard"};
  string label;
  string icon_name;
  vector<int32_t> child_ids;
  std::function<void()> on_click;
};

class DBusMenu final : public AdaptorInterfaces<com::canonical::dbusmenu_adaptor> {
  using PropertyMap = map<string, Variant>;
  using LayoutItem = Struct<int32_t, PropertyMap, vector<Variant>>;

  uint32_t revision{0};
  vector<DBusMenuItem> items;

 public:
  DBusMenu(IConnection& connection, const system_tray::Menu& root)
      : AdaptorInterfaces(connection, kMenuPath) {
    Populate(root);
    registerAdaptor();
  }
  ~DBusMenu() { unregisterAdaptor(); }

  void Rebuild(const system_tray::Menu& root) {
    Populate(root);
    revision++;
    emitLayoutUpdated(revision, 0);
  }

 private:
  int32_t Add(system_tray::MenuItem& node) {
    using namespace system_tray;
    int32_t id = (int32_t)items.size();
    items.emplace_back();
    switch (node.kind) {
      case MenuItem::SpacerKind:
        items[id].type = "separator";
        break;
      case MenuItem::ActionKind: {
        auto& a = *reinterpret_cast<system_tray::Action*>(&node);
        items[id].label = a.name;
        items[id].on_click = a.on_click;
        for (auto* link = a.icon; link; link = NextInChain(link)) {
          if (string name = TryGetIconName(link); !name.empty()) {
            items[id].icon_name = std::move(name);
            break;
          }
        }
        break;
      }
      case MenuItem::MenuKind: {
        auto& m = *reinterpret_cast<Menu*>(&node);
        items[id].label = m.name;
        for (auto* link = m.icon; link; link = NextInChain(link)) {
          if (string name = TryGetIconName(link); !name.empty()) {
            items[id].icon_name = std::move(name);
            break;
          }
        }
        for (auto* child : m.items) {
          int32_t child_id = Add(*child);
          items[id].child_ids.push_back(child_id);
        }
        break;
      }
    }
    return id;
  }

  void Populate(const system_tray::Menu& root) {
    items.clear();
    items.push_back({});
    for (auto* child : root.items) {
      int32_t child_id = Add(*child);
      items[0].child_ids.push_back(child_id);
    }
  }

 protected:
  uint32_t Version() override { return 3; }
  string TextDirection() override { return "ltr"; }
  string Status() override { return "normal"; }
  vector<string> IconThemePath() override { return {}; }

  static PropertyMap PropertiesFor(const DBusMenuItem& item, const vector<string>& names) {
    PropertyMap props;
    bool all = names.empty();
    auto want = [&](const std::string& n) {
      return all || std::find(names.begin(), names.end(), n) != names.end();
    };
    if (want("type") && item.type != "standard") props["type"] = Variant(item.type);
    if (want("label") && !item.label.empty()) props["label"] = Variant(item.label);
    if (want("icon-name") && !item.icon_name.empty()) props["icon-name"] = Variant(item.icon_name);
    if (want("children-display") && !item.child_ids.empty())
      props["children-display"] = Variant(std::string("submenu"));
    return props;
  }

  tuple<uint32_t, LayoutItem> GetLayout(const int32_t& parent_id, const int32_t& depth,
                                        const vector<string>& property_names) override {
    PropertyMap props;
    vector<Variant> children;
    if (parent_id < (int32_t)items.size()) {
      auto& item = items[parent_id];
      props = PropertiesFor(item, property_names);
      if (depth != 0) {
        for (auto child_id : item.child_ids) {
          auto [_, child] = GetLayout(child_id, depth - 1, property_names);
          children.push_back(Variant(child));
        }
      }
    }
    return {revision, LayoutItem{parent_id, props, children}};
  }

  vector<Struct<int32_t, PropertyMap>> GetGroupProperties(
      const vector<int32_t>& ids, const vector<string>& property_names) override {
    vector<Struct<int32_t, PropertyMap>> result;
    for (int32_t id : ids) {
      if (id < (int32_t)items.size()) {
        result.push_back({id, PropertiesFor(items[id], property_names)});
      }
    }
    return result;
  }

  Variant GetProperty(const int32_t& id, const string& name) override {
    if (id < (int32_t)items.size()) {
      auto props = PropertiesFor(items[id], {name});
      if (props.contains(name)) return props[name];
    }
    return Variant(string(""));
  }

  void Event(const int32_t& id, const string& event_id, const Variant& data,
             const uint32_t& timestamp) override {
    if (event_id == "clicked" && id < (int32_t)items.size() && items[id].on_click) {
      items[id].on_click();
    }
  }

  vector<int32_t> EventGroup(
      const vector<Struct<int32_t, string, Variant, uint32_t>>& events) override {
    for (auto& event : events) {
      Event(event.get<0>(), event.get<1>(), event.get<2>(), event.get<3>());
    }
    return {};
  }

  bool AboutToShow(const int32_t& id) override { return false; }
  tuple<vector<int32_t>, vector<int32_t>> AboutToShowGroup(const vector<int32_t>&) override {
    return {{}, {}};
  }
};

namespace system_tray {

struct Icon::Impl {
  IdPool::Handle uid;
  std::unique_ptr<IConnection> connection;
  std::unique_ptr<DBusMenu> menu;
  std::unique_ptr<StatusNotifierItem> sni;
  Impl(IdPool& pool) : uid(pool) {}
};

Icon::Icon(const Menu& root_menu, Action* default_item)
    : impl(std::make_unique<Impl>(IconUidPool())) {
  auto service_name = f("org.automat.pid-{}-{}", getpid(), impl->uid.id);
  impl->connection = createSessionBusConnection(ServiceName{service_name});
  impl->menu = std::make_unique<DBusMenu>(*impl->connection, root_menu);
  impl->sni =
      std::make_unique<StatusNotifierItem>(*impl->connection, root_menu.name, root_menu.icon,
                                           default_item ? default_item->on_click : Fn<void()>{});
  StatusNotifierWatcherProxy watcher{*impl->connection};
  watcher.RegisterStatusNotifierItem(service_name);
  impl->connection->enterEventLoopAsync();
}

Icon::~Icon() = default;

void Icon::Update(const Menu& root_menu, Action*) {
  if (!impl || !impl->menu) return;
  impl->menu->Rebuild(root_menu);
}

}  // namespace system_tray

#elif defined(_WIN32)

namespace {

std::map<UINT, system_tray::Icon::Impl*>& IconsByUid() {
  static std::map<UINT, system_tray::Icon::Impl*> m;
  return m;
}

HWND MainHwnd() { return static_cast<Win32Window*>(ui::root_widget->window.get())->hwnd; }

std::wstring Utf8ToWide(StrView s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

HICON MakeHIconFromImage(const sk_sp<SkImage>& image, int size) {
  if (!image) return nullptr;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC screen_dc = GetDC(nullptr);
  void* dib_bits = nullptr;
  HBITMAP color_bmp = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
  ReleaseDC(nullptr, screen_dc);
  if (!color_bmp || !dib_bits) {
    if (color_bmp) DeleteObject(color_bmp);
    return nullptr;
  }

  auto image_info = SkImageInfo::Make(size, size, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
  SkPixmap sk_pixmap(image_info, dib_bits, size * 4);
  image->scalePixels(sk_pixmap, SkSamplingOptions(SkCubicResampler::CatmullRom()));

  HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, nullptr);

  ICONINFO icon_info = {};
  icon_info.fIcon = TRUE;
  icon_info.hbmColor = color_bmp;
  icon_info.hbmMask = mask_bmp;
  HICON icon = CreateIconIndirect(&icon_info);

  DeleteObject(color_bmp);
  DeleteObject(mask_bmp);
  return icon;
}

// Attempts to load a single chain link as an HICON at the given size. Returns null if this
// link's kind isn't usable on Windows, or if the load fails. Caller owns the returned HICON.
HICON TryLoadHIcon(system_tray::MenuIcon* link, int size) {
  using namespace system_tray;
  if (!link) return nullptr;
  if (link->kind == MenuIcon::WindowsStockIconKind) {
    auto* w = reinterpret_cast<WindowsStockIcon*>(link);
    SHSTOCKICONINFO info = {};
    info.cbSize = sizeof(info);
    UINT flags = SHGSI_ICON | (size <= 16 ? SHGSI_SMALLICON : SHGSI_LARGEICON);
    if (SUCCEEDED(SHGetStockIconInfo((SHSTOCKICONID)w->shell_stock_icon_id, flags, &info)) &&
        info.hIcon) {
      return info.hIcon;
    }
    return nullptr;
  }
  if (link->kind == MenuIcon::SkiaIconKind) {
    auto* s = reinterpret_cast<SkiaIcon*>(link);
    return MakeHIconFromImage(s->image, size);
  }
  return nullptr;
}

HBITMAP HIconToMenuBitmap(HICON hicon, int size) {
  if (!hicon) return nullptr;
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  HDC screen_dc = GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp && bits) {
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HGDIOBJ old = SelectObject(mem_dc, bmp);
    DrawIconEx(mem_dc, 0, 0, hicon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
  }
  ReleaseDC(nullptr, screen_dc);
  return bmp;
}

void AttachBitmap(HMENU hmenu, UINT id_or_pos, bool by_position, HBITMAP bmp) {
  if (!bmp) return;
  MENUITEMINFOW mii = {};
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_BITMAP;
  mii.hbmpItem = bmp;
  SetMenuItemInfoW(hmenu, id_or_pos, by_position ? TRUE : FALSE, &mii);
}

void BuildMenu(HMENU hmenu, const system_tray::Menu& menu, Vec<Fn<void()>>& handlers,
               Vec<HBITMAP>& bitmaps, system_tray::Action* default_target) {
  static const int menu_icon_size = GetSystemMetrics(SM_CXSMICON);
  for (auto* item : menu.items) {
    switch (item->kind) {
      case system_tray::MenuItem::SpacerKind:
        AppendMenuW(hmenu, MF_SEPARATOR, 0, nullptr);
        break;
      case system_tray::MenuItem::ActionKind: {
        auto& a = *reinterpret_cast<system_tray::Action*>(item);
        handlers.push_back(a.on_click);
        UINT cmd = (UINT)handlers.size();
        auto wide = Utf8ToWide(a.name);
        AppendMenuW(hmenu, MF_STRING, cmd, wide.c_str());
        if (&a == default_target) SetMenuDefaultItem(hmenu, cmd, FALSE);
        for (auto* link = a.icon; link; link = NextInChain(link)) {
          HICON hicon = TryLoadHIcon(link, menu_icon_size);
          if (!hicon) continue;
          HBITMAP bmp = HIconToMenuBitmap(hicon, menu_icon_size);
          DestroyIcon(hicon);
          if (!bmp) continue;
          bitmaps.push_back(bmp);
          AttachBitmap(hmenu, cmd, /*by_position=*/false, bmp);
          break;
        }
        break;
      }
      case system_tray::MenuItem::MenuKind: {
        auto& m = *reinterpret_cast<system_tray::Menu*>(item);
        HMENU sub = CreatePopupMenu();
        BuildMenu(sub, m, handlers, bitmaps, default_target);
        auto wide = Utf8ToWide(m.name);
        AppendMenuW(hmenu, MF_POPUP, (UINT_PTR)sub, wide.c_str());
        UINT pos = (UINT)GetMenuItemCount(hmenu) - 1;
        for (auto* link = m.icon; link; link = NextInChain(link)) {
          HICON hicon = TryLoadHIcon(link, menu_icon_size);
          if (!hicon) continue;
          HBITMAP bmp = HIconToMenuBitmap(hicon, menu_icon_size);
          DestroyIcon(hicon);
          if (!bmp) continue;
          bitmaps.push_back(bmp);
          AttachBitmap(hmenu, pos, /*by_position=*/true, bmp);
          break;
        }
        break;
      }
    }
  }
}

}  // namespace

namespace system_tray {

struct Icon::Impl {
  IdPool::Handle uid;
  HMENU hmenu = nullptr;
  HICON hicon = nullptr;
  Vec<Fn<void()>> action_handlers;
  Vec<HBITMAP> menu_bitmaps;
  Fn<void()> on_activate;
  NOTIFYICONDATAW data = {};

  Impl(IdPool& pool) : uid(pool) {}
};

static int TraySize() { return GetSystemMetrics(SM_CXSMICON); }

Icon::Icon(const Menu& root_menu, Action* default_item)
    : impl(std::make_unique<Impl>(IconUidPool())) {
  IconsByUid()[(UINT)impl->uid.id] = impl.get();

  impl->hmenu = CreatePopupMenu();
  BuildMenu(impl->hmenu, root_menu, impl->action_handlers, impl->menu_bitmaps, default_item);
  impl->on_activate = default_item ? default_item->on_click : Fn<void()>{};
  for (auto* link = root_menu.icon; link; link = NextInChain(link)) {
    if ((impl->hicon = TryLoadHIcon(link, TraySize()))) break;
  }

  auto& data = impl->data;
  data.cbSize = sizeof(data);
  data.hWnd = MainHwnd();
  data.uID = (UINT)impl->uid.id;
  data.uFlags = NIF_MESSAGE | NIF_TIP | (impl->hicon ? NIF_ICON : 0);
  data.uCallbackMessage = kSystemTrayMessage;
  data.hIcon = impl->hicon;
  auto wide_tip = Utf8ToWide(root_menu.name);
  wcsncpy_s(data.szTip, wide_tip.c_str(), _TRUNCATE);

  if (!Shell_NotifyIconW(NIM_ADD, &data)) {
    ERROR << "Failed to add system tray icon: " << GetLastError();
    data.cbSize = 0;
    return;
  }
  data.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &data);
}

Icon::~Icon() {
  if (!impl) return;
  if (impl->data.cbSize != 0) Shell_NotifyIconW(NIM_DELETE, &impl->data);
  if (impl->hmenu) DestroyMenu(impl->hmenu);
  for (HBITMAP b : impl->menu_bitmaps) DeleteObject(b);
  if (impl->hicon) DestroyIcon(impl->hicon);
  IconsByUid().erase((UINT)impl->uid.id);
}

void Icon::Update(const Menu& root_menu, Action* default_item) {
  if (!impl) return;
  if (impl->hmenu) DestroyMenu(impl->hmenu);
  for (HBITMAP b : impl->menu_bitmaps) DeleteObject(b);
  impl->menu_bitmaps.clear();
  impl->hmenu = CreatePopupMenu();
  impl->action_handlers.clear();
  BuildMenu(impl->hmenu, root_menu, impl->action_handlers, impl->menu_bitmaps, default_item);
  impl->on_activate = default_item ? default_item->on_click : Fn<void()>{};

  HICON new_hicon = nullptr;
  for (auto* link = root_menu.icon; link; link = NextInChain(link)) {
    if ((new_hicon = TryLoadHIcon(link, TraySize()))) break;
  }
  auto wide_tip = Utf8ToWide(root_menu.name);
  auto& data = impl->data;
  data.uFlags = NIF_MESSAGE | NIF_TIP | (new_hicon ? NIF_ICON : 0);
  data.hIcon = new_hicon;
  wcsncpy_s(data.szTip, wide_tip.c_str(), _TRUNCATE);
  if (data.cbSize != 0) Shell_NotifyIconW(NIM_MODIFY, &data);

  if (impl->hicon) DestroyIcon(impl->hicon);
  impl->hicon = new_hicon;
}

}  // namespace system_tray

void OnSystemTrayMessage(unsigned event, int mouse_x, int mouse_y, unsigned icon_uid) {
  auto it = IconsByUid().find(icon_uid);
  if (it == IconsByUid().end()) return;
  auto& impl = *it->second;
  switch (event) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
      if (impl.on_activate) impl.on_activate();
      return;
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP: {
      if (!impl.hmenu) return;
      HWND hwnd = MainHwnd();
      SetForegroundWindow(hwnd);
      UINT cmd = TrackPopupMenu(impl.hmenu,
                                TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                mouse_x, mouse_y, 0, hwnd, nullptr);
      PostMessageW(hwnd, WM_NULL, 0, 0);
      if (cmd >= 1 && cmd <= impl.action_handlers.size()) {
        auto& fn = impl.action_handlers[cmd - 1];
        if (fn) fn();
      }
      return;
    }
  }
}

#else  // other platforms: no-op

namespace system_tray {

struct Icon::Impl {};
Icon::Icon(const Menu&, Action*) : impl(std::make_unique<Impl>()) {}
Icon::~Icon() = default;
void Icon::Update(const Menu&, Action*) {}

}  // namespace system_tray

#endif

}  // namespace automat
