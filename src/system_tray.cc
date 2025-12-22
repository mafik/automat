// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#ifdef __linux__
#include <sdbus-c++/sdbus-c++.h>

#include "../build/generated/com.canonical.dbusmenu_adaptor.hh"
#include "../build/generated/org.kde.StatusNotifierItem_adaptor.hh"
#include "../build/generated/org.kde.StatusNotifierWatcher_proxy.hh"
#endif

#include <vector>

#include "format.hh"
#include "log.hh"

namespace automat {

#ifdef __linux__

using std::map;
using std::string;
using std::tuple;
using std::vector;
using namespace sdbus;

const ObjectPath kStatusNotifierPath{"/StatusNotifierItem"};
const ObjectPath kMenuPath{"/MenuBar"};

//////////////////////////////////
/// org.kde.StatusNotifierWatcher proxy
//////////////////////////////////

class StatusNotifierWatcher final : public ProxyInterfaces<org::kde::StatusNotifierWatcher_proxy> {
 public:
  StatusNotifierWatcher(IConnection& connection)
      : ProxyInterfaces(connection, ServiceName{"org.kde.StatusNotifierWatcher"},
                        ObjectPath{"/StatusNotifierWatcher"}) {
    registerProxy();
  }

  ~StatusNotifierWatcher() { unregisterProxy(); }

 protected:
  void onStatusNotifierItemRegistered(const string& item) override {
    LOG << "StatusNotifierWatcher::onStatusNotifierItemRegistered(" << item << ")";
  }
  void onStatusNotifierItemUnregistered(const string& item) override {
    LOG << "StatusNotifierWatcher::onStatusNotifierItemUnregistered(" << item << ")";
  }
  void onStatusNotifierHostRegistered() override {
    LOG << "StatusNotifierWatcher::onStatusNotifierHostRegistered()";
  }
};

/////////////////////////////////////////
/// org.kde.StatusNotifierItem adaptor
/////////////////////////////////////////

class StatusNotifierItem final : public AdaptorInterfaces<org::kde::StatusNotifierItem_adaptor> {
  // Icon pixmap type: (width, height, ARGB32 data)
  using IconPixmap_t = Struct<int32_t, int32_t, vector<uint8_t>>;
  using IconPixmapList = vector<IconPixmap_t>;

  // Tooltip type: (icon_name, icon_pixmaps, title, description)
  using Tooltip = std::tuple<string, IconPixmapList, string, string>;

 public:
  StatusNotifierItem(IConnection& connection) : AdaptorInterfaces(connection, kStatusNotifierPath) {
    registerAdaptor();
  }
  ~StatusNotifierItem() { unregisterAdaptor(); }

 protected:
  // Property getters
  string Category() override { return "ApplicationStatus"; }
  string Id() override { return "automat"; }
  string Title() override { return "Automat"; }
  string Status() override { return "Active"; }
  uint32_t WindowId() override { return 0; }
  string IconName() override { return "format-text-rich-symbolic"; }
  IconPixmapList IconPixmap() override { return {}; }
  string OverlayIconName() override { return ""; }
  IconPixmapList OverlayIconPixmap() override { return {}; }
  string AttentionIconName() override { return ""; }
  IconPixmapList AttentionIconPixmap() override { return {}; }
  string AttentionMovieName() override { return ""; }
  Struct<string, IconPixmapList, string, string> ToolTip() override {
    // icon name, icon pixmaps, title, description
    return {"", {}, "Automat", ""};
  }
  bool ItemIsMenu() override { return false; }
  ObjectPath Menu() override { return kMenuPath; }

  // Methods
  void ContextMenu(const int32_t& x, const int32_t& y) override {
    LOG << "StatusNotifierItem::ContextMenu(" << x << ", " << y << ")";
  }
  void Activate(const int32_t& x, const int32_t& y) override {
    LOG << "StatusNotifierItem::Activate(" << x << ", " << y << ")";
  }
  void SecondaryActivate(const int32_t& x, const int32_t& y) override {
    LOG << "StatusNotifierItem::SecondaryActivate(" << x << ", " << y << ")";
  }
  void Scroll(const int32_t& delta, const string& orientation) override {
    LOG << "StatusNotifierItem::Scroll(" << delta << ", " << orientation << ")";
  }
};

////////////////////////////////////////
/// com.canonical.dbusmenu adaptor
////////////////////////////////////////

// Menu item properties
struct MenuItemProperties {
  string type{"standard"};  // "standard" or "separator"
  string label;
  bool enabled{true};
  bool visible{true};
  string icon_name;
  int toggle_state = -1;  // 1, 0, or -1
  string toggle_type;     // "checkmark", "radio" or ""
  string shortcut;
  vector<int32_t> child_ids;
};

class DBusMenu final : public AdaptorInterfaces<com::canonical::dbusmenu_adaptor> {
  using PropertyMap = map<string, Variant>;
  using LayoutItem = Struct<int32_t, PropertyMap, vector<Variant>>;

  uint32_t revision{0};
  vector<MenuItemProperties> items;

 public:
  DBusMenu(IConnection& connection) : AdaptorInterfaces(connection, kMenuPath) {
    registerAdaptor();
    items.push_back({});  // Root item
    // items.push_back({.type = "standard", .label = "Show", .icon_name = "view-reveal-symbolic"});
    items.push_back({.type = "standard", .label = "Hide", .icon_name = "view-conceal-symbolic"});
    items.push_back({.type = "separator", .label = "", .icon_name = "", .shortcut = ""});
    items.push_back(
        {.type = "standard", .label = "Quit", .icon_name = "application-exit-symbolic"});
    items[0].child_ids.push_back(1);
    items[0].child_ids.push_back(2);
    items[0].child_ids.push_back(3);
  }
  ~DBusMenu() { unregisterAdaptor(); }

 protected:
  // Property getters
  uint32_t Version() override {
    return 3;  // DBusMenu protocol version
  }
  string TextDirection() override { return "ltr"; }
  string Status() override { return "normal"; }
  vector<string> IconThemePath() override { return {}; }

  static PropertyMap GetPropertiesForItem(const MenuItemProperties& item,
                                          const vector<string>& propertyNames) {
    PropertyMap props;

    bool all = propertyNames.empty();

    for (auto& prop_name : propertyNames) {
      LOG << "Requested " << prop_name;
    }
    if (all) {
      LOG << "Requested ALL";
    }

    auto shouldInclude = [&](const std::string& name) {
      return all ||
             std::find(propertyNames.begin(), propertyNames.end(), name) != propertyNames.end();
    };

    if (shouldInclude("type") && item.type != "standard") {
      props["type"] = sdbus::Variant(item.type);
    }
    if (shouldInclude("label") && !item.label.empty()) {
      props["label"] = sdbus::Variant(item.label);
    }
    if (shouldInclude("enabled") && !item.enabled) {
      props["enabled"] = sdbus::Variant(item.enabled);
    }
    if (shouldInclude("visible") && !item.visible) {
      props["visible"] = sdbus::Variant(item.visible);
    }
    if (shouldInclude("icon-name") && !item.icon_name.empty()) {
      props["icon-name"] = sdbus::Variant(item.icon_name);
    }
    if (shouldInclude("toggle-state") && item.toggle_state != -1) {
      props["toggle-state"] = sdbus::Variant(item.toggle_state);
    }
    if (shouldInclude("toggle-type")) {
      props["toggle-type"] = sdbus::Variant(item.toggle_type);
    }
    if (shouldInclude("shortcut") && !item.shortcut.empty()) {
      // Shortcut format: array of array of strings
      std::vector<std::vector<std::string>> shortcut = {{item.shortcut}};
      props["shortcut"] = sdbus::Variant(shortcut);
    }
    if (shouldInclude("children-display") && !item.child_ids.empty()) {
      props["children-display"] = sdbus::Variant(std::string("submenu"));
    }

    return props;
  }

  tuple<uint32_t, LayoutItem> GetLayout(const int32_t& parent_id, const int32_t& depth,
                                        const vector<string>& property_names) override {
    LOG << "DBusMenu::GetLayout(" << parent_id << ", " << depth << ")";
    PropertyMap props;
    vector<Variant> children;
    if (parent_id < items.size()) {
      auto& item = items[parent_id];
      props = GetPropertiesForItem(item, property_names);
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
      if (id < items.size()) {
        result.push_back({id, GetPropertiesForItem(items[id], property_names)});
      }
    }
    return result;
  }

  Variant GetProperty(const int32_t& id, const string& name) override {
    if (id >= items.size()) {
      auto props = GetPropertiesForItem(items[id], {name});
      if (props.contains(name)) {
        return props[name];
      }
    }
    return Variant(string(""));
  }

  void Event(const int32_t& id, const string& event_id, const Variant& data,
             const uint32_t& timestamp) override {
    LOG << "DBusMenu::Event(" << id << ", " << event_id << ", " << data.peekValueType() << ", "
        << timestamp << ")";
  }

  vector<int32_t> EventGroup(
      const vector<Struct<int32_t, string, Variant, uint32_t>>& events) override {
    for (auto& event : events) {
      Event(event.get<0>(), event.get<1>(), event.get<2>(), event.get<3>());
    }
    return {};
  }

  bool AboutToShow(const int32_t& id) override { return false; }

  tuple<vector<int32_t>, vector<int32_t>> AboutToShowGroup(const vector<int32_t>& ids) override {
    std::vector<int32_t> needs_update;
    std::vector<int32_t> errors;

    for (int32_t id : ids) {
      if (id < items.size()) {
        if (AboutToShow(id)) {
          needs_update.push_back(id);
        }
      } else {
        errors.push_back(id);
      }
    }

    return {needs_update, errors};
  }
};

std::unique_ptr<IConnection> dbus_connection;

std::unique_ptr<DBusMenu> system_tray_menu;
std::unique_ptr<StatusNotifierItem> status_notifier_item;

void InitSystemTray() {
  pid_t pid = getpid();
  auto service_name = f("org.automat.pid-{}", pid);
  dbus_connection = createSessionBusConnection(ServiceName{service_name});

  system_tray_menu = std::make_unique<DBusMenu>(*dbus_connection);
  status_notifier_item = std::make_unique<StatusNotifierItem>(*dbus_connection);
  StatusNotifierWatcher status_notifier_watcher{*dbus_connection};
  status_notifier_watcher.RegisterStatusNotifierItem(service_name);

  dbus_connection->enterEventLoopAsync();
}
#else
void InitSystemTray() {}
#endif

}  // namespace automat
