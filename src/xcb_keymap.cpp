// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "xcb_keymap.hpp"

#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

#include <ankerl/unordered_dense.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <bit>

#include "format.hpp"
#include "int.hpp"
#include "vec.hpp"
#include "xcb.hpp"

using namespace automat;

namespace xcb {

namespace {

constexpr int kRealModCount = 8;
constexpr int kVirtualModCount = 16;
constexpr int kIndicatorCount = 32;
constexpr int kKeycodeCount = 256;
constexpr U32 kAllRealMods = 0xff;
constexpr U8 kNoVirtualMod = 0xff;

constexpr U16 kMapParts = XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS |
                          XCB_XKB_MAP_PART_MODIFIER_MAP | XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
                          XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS |
                          XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;

constexpr U32 kNameDetails =
    XCB_XKB_NAME_DETAIL_KEYCODES | XCB_XKB_NAME_DETAIL_SYMBOLS | XCB_XKB_NAME_DETAIL_TYPES |
    XCB_XKB_NAME_DETAIL_COMPAT | XCB_XKB_NAME_DETAIL_KEY_TYPE_NAMES |
    XCB_XKB_NAME_DETAIL_KT_LEVEL_NAMES | XCB_XKB_NAME_DETAIL_INDICATOR_NAMES |
    XCB_XKB_NAME_DETAIL_KEY_NAMES | XCB_XKB_NAME_DETAIL_KEY_ALIASES |
    XCB_XKB_NAME_DETAIL_VIRTUAL_MOD_NAMES | XCB_XKB_NAME_DETAIL_GROUP_NAMES;

constexpr U32 kRequiredNameDetails =
    XCB_XKB_NAME_DETAIL_KEY_TYPE_NAMES | XCB_XKB_NAME_DETAIL_KT_LEVEL_NAMES |
    XCB_XKB_NAME_DETAIL_KEY_NAMES | XCB_XKB_NAME_DETAIL_VIRTUAL_MOD_NAMES;

constexpr const char* kRealModNames[kRealModCount] = {"Shift", "Lock", "Control", "Mod1",
                                                      "Mod2",  "Mod3", "Mod4",    "Mod5"};

constexpr struct {
  U32 wire_bit;
  const char* name;
} kControlNames[] = {
    {XCB_XKB_BOOL_CTRL_REPEAT_KEYS, "RepeatKeys"},
    {XCB_XKB_BOOL_CTRL_SLOW_KEYS, "SlowKeys"},
    {XCB_XKB_BOOL_CTRL_BOUNCE_KEYS, "BounceKeys"},
    {XCB_XKB_BOOL_CTRL_STICKY_KEYS, "StickyKeys"},
    {XCB_XKB_BOOL_CTRL_MOUSE_KEYS, "MouseKeys"},
    {XCB_XKB_BOOL_CTRL_MOUSE_KEYS_ACCEL, "MouseKeysAccel"},
    {XCB_XKB_BOOL_CTRL_ACCESS_X_KEYS, "AccessXKeys"},
    {XCB_XKB_BOOL_CTRL_ACCESS_X_TIMEOUT_MASK, "AccessXTimeout"},
    {XCB_XKB_BOOL_CTRL_ACCESS_X_FEEDBACK_MASK, "AccessXFeedback"},
    {XCB_XKB_BOOL_CTRL_AUDIBLE_BELL_MASK, "AudibleBell"},
    {XCB_XKB_BOOL_CTRL_IGNORE_GROUP_LOCK_MASK, "IgnoreGroupLock"},
};

constexpr U32 kNamedControls = [] {
  U32 mask = 0;
  for (auto& control : kControlNames) mask |= control.wire_bit;
  return mask;
}();

constexpr const char* kMatchNames[] = {"NoneOf", "AnyOfOrNone", "AnyOf", "AllOf", "Exactly"};

constexpr const char* kStateComponentNames[] = {"base", "latched", "locked", "effective"};
constexpr U8 kEffectiveStateComponent = 8;

struct KeyType {
  xcb_xkb_key_type_t* wire = nullptr;
  xcb_atom_t name = XCB_ATOM_NONE;
  xcb_atom_t* level_names = nullptr;
};

struct Key {
  xcb_xkb_key_sym_map_t* sym_map = nullptr;
  xcb_xkb_action_t* actions = nullptr;
  const char* name = nullptr;
  U8 explicit_components = 0;
  U8 real_mods = 0;
  U16 virtual_mods = 0;
  bool repeats = false;
};

struct ServerKeymap {
  xcb_xkb_get_map_reply_t* map;
  xcb_xkb_get_names_reply_t* names;
  xcb_xkb_get_compat_map_reply_t* compat;
  xcb_xkb_get_indicator_map_reply_t* indicators;
  xcb_xkb_get_controls_reply_t* controls;
  xcb_xkb_get_names_value_list_t name_lists = {};
  ankerl::unordered_dense::map<xcb_atom_t, Str> atom_names;
  StrView mod_names[kRealModCount + kVirtualModCount] = {};
  bool mod_is_virtual[kRealModCount + kVirtualModCount] = {};
  int mod_count = kRealModCount;
  Vec<KeyType> types;
  Key keys[kKeycodeCount];
  xcb_xkb_indicator_map_t* indicator_maps[kIndicatorCount] = {};
  xcb_atom_t indicator_names[kIndicatorCount] = {};
  int indicator_count = 0;
};

}  // namespace

static bool ReadMap(ServerKeymap& keymap) {
  xcb_xkb_get_map_reply_t* reply = keymap.map;
  if ((reply->present & kMapParts) != kMapParts) return false;
  xcb_xkb_get_map_map_t parts = {};
  xcb_xkb_get_map_map_unpack(xcb_xkb_get_map_map(reply), reply->nTypes, reply->nKeySyms,
                             reply->nKeyActions, reply->totalActions, reply->totalKeyBehaviors,
                             reply->virtualMods, reply->totalKeyExplicit, reply->totalModMapKeys,
                             reply->totalVModMapKeys, reply->present, &parts);

  xcb_xkb_key_type_iterator_t types = xcb_xkb_get_map_map_types_rtrn_iterator(reply, &parts);
  for (; types.rem; xcb_xkb_key_type_next(&types)) {
    keymap.types.push_back({.wire = types.data});
  }

  if (reply->firstKeySym + reply->nKeySyms > kKeycodeCount) return false;
  xcb_xkb_key_sym_map_iterator_t sym_maps = xcb_xkb_get_map_map_syms_rtrn_iterator(reply, &parts);
  for (int i = 0; sym_maps.rem; ++i, xcb_xkb_key_sym_map_next(&sym_maps)) {
    xcb_xkb_key_sym_map_t* sym_map = sym_maps.data;
    int group_count = sym_map->groupInfo & 0x0f;
    if (group_count > 4 || sym_map->nSyms != sym_map->width * group_count) return false;
    for (int group = 0; group < group_count; ++group) {
      if (sym_map->kt_index[group] >= keymap.types.size()) return false;
    }
    keymap.keys[reply->firstKeySym + i].sym_map = sym_map;
  }

  if (reply->firstKeyAction + reply->nKeyActions > kKeycodeCount) return false;
  int action_count = xcb_xkb_get_map_map_acts_rtrn_count_length(reply, &parts);
  int actions_used = 0;
  for (int i = 0; i < action_count; ++i) {
    U8 count = parts.acts_rtrn_count[i];
    if (count == 0) continue;
    Key& key = keymap.keys[reply->firstKeyAction + i];
    if (!key.sym_map || count != key.sym_map->nSyms) return false;
    if (actions_used + count > reply->totalActions) return false;
    key.actions = parts.acts_rtrn_acts + actions_used;
    actions_used += count;
  }

  for (int i = 0; i < kVirtualModCount; ++i) {
    keymap.mod_is_virtual[kRealModCount + i] = reply->virtualMods & (1u << i);
  }

  int explicit_count = xcb_xkb_get_map_map_explicit_rtrn_length(reply, &parts);
  for (int i = 0; i < explicit_count; ++i) {
    xcb_xkb_set_explicit_t& entry = parts.explicit_rtrn[i];
    keymap.keys[entry.keycode].explicit_components = entry.explicit_;
  }

  int modmap_count = xcb_xkb_get_map_map_modmap_rtrn_length(reply, &parts);
  for (int i = 0; i < modmap_count; ++i) {
    xcb_xkb_key_mod_map_t& entry = parts.modmap_rtrn[i];
    keymap.keys[entry.keycode].real_mods = entry.mods;
  }

  int vmodmap_count = xcb_xkb_get_map_map_vmodmap_rtrn_length(reply, &parts);
  for (int i = 0; i < vmodmap_count; ++i) {
    xcb_xkb_key_v_mod_map_t& entry = parts.vmodmap_rtrn[i];
    keymap.keys[entry.keycode].virtual_mods = entry.vmods;
  }
  return true;
}

static bool ReadNames(ServerKeymap& keymap) {
  xcb_xkb_get_names_reply_t* reply = keymap.names;
  if ((reply->which & kRequiredNameDetails) != kRequiredNameDetails) return false;
  if (reply->nTypes != keymap.types.size()) return false;
  xcb_xkb_get_names_value_list_t& lists = keymap.name_lists;
  xcb_xkb_get_names_value_list_unpack(xcb_xkb_get_names_value_list(reply), reply->nTypes,
                                      reply->indicators, reply->virtualMods, reply->groupNames,
                                      reply->nKeys, reply->nKeyAliases, reply->nRadioGroups,
                                      reply->which, &lists);

  xcb_atom_t* level_names = lists.ktLevelNames;
  for (int i = 0; i < reply->nTypes; ++i) {
    KeyType& type = keymap.types[i];
    if (lists.nLevelsPerType[i] != type.wire->numLevels) return false;
    type.name = lists.typeNames[i];
    type.level_names = level_names;
    level_names += type.wire->numLevels;
  }

  for (int i = 0; i < kRealModCount; ++i) {
    keymap.mod_names[i] = kRealModNames[i];
  }
  keymap.mod_count = kRealModCount + std::bit_width(reply->virtualMods);

  if (reply->firstKey + reply->nKeys > kKeycodeCount) return false;
  for (int i = 0; i < reply->nKeys; ++i) {
    keymap.keys[reply->firstKey + i].name = lists.keyNames[i].name;
  }

  keymap.indicator_count = std::bit_width(keymap.indicators->which);
  xcb_xkb_indicator_map_t* indicator_map = xcb_xkb_get_indicator_map_maps(keymap.indicators);
  xcb_atom_t* indicator_name = lists.indicatorNames;
  for (int i = 0; i < kIndicatorCount; ++i) {
    if (keymap.indicators->which & (1u << i)) keymap.indicator_maps[i] = indicator_map++;
    if (reply->indicators & (1u << i)) keymap.indicator_names[i] = *indicator_name++;
  }

  for (int kc = keymap.map->minKeyCode; kc <= keymap.map->maxKeyCode; ++kc) {
    keymap.keys[kc].repeats = keymap.controls->perKeyRepeat[kc / 8] & (1u << (kc % 8));
  }
  return true;
}

static bool ReadAtomNames(ServerKeymap& keymap) {
  xcb_xkb_get_names_reply_t* reply = keymap.names;
  xcb_xkb_get_names_value_list_t& lists = keymap.name_lists;
  Vec<xcb_atom_t> atoms;
  Vec<xcb_get_atom_name_cookie_t> cookies;
  auto RequestAtomName = [&](xcb_atom_t atom) {
    if (atom == XCB_ATOM_NONE || !keymap.atom_names.emplace(atom, Str()).second) return;
    atoms.push_back(atom);
    cookies.push_back(xcb_get_atom_name(connection, atom));
  };
  RequestAtomName(lists.keycodesName);
  RequestAtomName(lists.typesName);
  RequestAtomName(lists.compatName);
  RequestAtomName(lists.symbolsName);
  for (KeyType& type : keymap.types) {
    RequestAtomName(type.name);
    for (int level = 0; level < type.wire->numLevels; ++level) {
      RequestAtomName(type.level_names[level]);
    }
  }
  for (xcb_atom_t name : keymap.indicator_names) {
    RequestAtomName(name);
  }
  int virtual_mod_names = std::popcount(reply->virtualMods);
  for (int i = 0; i < virtual_mod_names; ++i) {
    RequestAtomName(lists.virtualModNames[i]);
  }
  int group_names = std::popcount(reply->groupNames);
  for (int i = 0; i < group_names; ++i) {
    RequestAtomName(lists.groups[i]);
  }

  bool all_replied = true;
  for (size_t i = 0; i < atoms.size(); ++i) {
    std::unique_ptr<xcb_get_atom_name_reply_t, crt::DeleteWithFree> atom_reply(
        xcb_get_atom_name_reply(connection, cookies[i], nullptr));
    if (!atom_reply) {
      all_replied = false;
      continue;
    }
    keymap.atom_names[atoms[i]] = Str(xcb_get_atom_name_name(atom_reply.get()),
                                      xcb_get_atom_name_name_length(atom_reply.get()));
  }
  if (!all_replied) return false;

  xcb_atom_t* virtual_mod_name = lists.virtualModNames;
  for (int i = 0; i < kVirtualModCount; ++i) {
    if (reply->virtualMods & (1u << i)) {
      keymap.mod_names[kRealModCount + i] = keymap.atom_names[*virtual_mod_name++];
    }
  }
  return true;
}

static bool InterpretMatchesAndModifiersAreKnown(const ServerKeymap& keymap) {
  xcb_xkb_sym_interpret_t* interprets = xcb_xkb_get_compat_map_si_rtrn(keymap.compat);
  for (int i = 0; i < keymap.compat->nSIRtrn; ++i) {
    if ((size_t)(interprets[i].match & XCB_XKB_SYM_INTERP_MATCH_OP_MASK) >=
        std::size(kMatchNames)) {
      return false;
    }
    if (interprets[i].virtualMod != kNoVirtualMod && interprets[i].virtualMod >= kVirtualModCount) {
      return false;
    }
  }
  return true;
}

static StrView AtomText(const ServerKeymap& keymap, xcb_atom_t atom) {
  auto it = keymap.atom_names.find(atom);
  if (it == keymap.atom_names.end()) return {};
  return it->second;
}

static bool IsLegalInMapName(U8 c) {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  if (StrView("()*-/?_").find(c) != StrView::npos) return true;
  return c >= 0xc0 && c != 0xd7 && c != 0xf7;
}

static Str EscapeMapName(StrView name) {
  Str escaped(name);
  for (char& c : escaped) {
    if (!IsLegalInMapName(c)) c = '_';
  }
  return escaped;
}

static void WriteSectionHeader(const ServerKeymap& keymap, Str& text, StrView section,
                               xcb_atom_t name) {
  if (name == XCB_ATOM_NONE) {
    text += f("{} {{\n", section);
  } else {
    text += f("{} \"{}\" {{\n", section, EscapeMapName(AtomText(keymap, name)));
  }
}

static Str ModMaskText(const ServerKeymap& keymap, U32 mask) {
  if (mask == 0) return "none";
  if (mask == kAllRealMods) return "all";
  if (mask >> keymap.mod_count) return f("0x{:x}", mask);
  Str text;
  for (int i = 0; i < keymap.mod_count; ++i) {
    if (!(mask & (1u << i))) continue;
    if (!text.empty()) text += '+';
    text += keymap.mod_names[i];
  }
  return text;
}

static Str ControlMaskText(U32 controls) {
  if (controls == 0) return "none";
  if (controls == kNamedControls) return "all";
  Str text;
  for (auto& [wire_bit, name] : kControlNames) {
    if (!(controls & wire_bit)) continue;
    if (!text.empty()) text += '+';
    text += name;
  }
  return text;
}

static U8 StateComponentsFromWire(U8 which) {
  U8 components = which & 0x0f;
  if (which & XCB_XKB_IM_MODS_WHICH_USE_COMPAT) components |= kEffectiveStateComponent;
  return components;
}

static Str StateComponentsText(U8 components) {
  Str text;
  for (int i = 0; i < 4; ++i) {
    if (!(components & (1u << i))) continue;
    if (!text.empty()) text += '+';
    text += kStateComponentNames[i];
  }
  return text;
}

static Str KeyNameText(const char* name) {
  if (!name) return "<>";
  return f("<{}>", StrView(name, strnlen(name, XCB_XKB_CONST_KEY_NAME_LENGTH)));
}

static Str KeysymText(xkb_keysym_t keysym) {
  char name[64];
  xkb_keysym_get_name(keysym, name, sizeof(name));
  return name;
}

static StrView LockAffectText(U8 flags, bool show_both) {
  switch (flags & (XCB_XKB_SA_ISO_LOCK_FLAG_NO_LOCK | XCB_XKB_SA_ISO_LOCK_FLAG_NO_UNLOCK)) {
    case XCB_XKB_SA_ISO_LOCK_FLAG_NO_UNLOCK:
      return ",affect=lock";
    case XCB_XKB_SA_ISO_LOCK_FLAG_NO_LOCK:
      return ",affect=unlock";
    case XCB_XKB_SA_ISO_LOCK_FLAG_NO_LOCK | XCB_XKB_SA_ISO_LOCK_FLAG_NO_UNLOCK:
      return ",affect=neither";
  }
  return show_both ? ",affect=both" : "";
}

static StrView SignText(bool absolute, int value) { return !absolute && value >= 0 ? "+" : ""; }

static Str ActionText(const ServerKeymap& keymap, const xcb_xkb_action_t& action) {
  switch (action.type) {
    case XCB_XKB_SA_TYPE_SET_MODS:
    case XCB_XKB_SA_TYPE_LATCH_MODS:
    case XCB_XKB_SA_TYPE_LOCK_MODS: {
      const xcb_xkb_sa_set_mods_t& mods = action.setmods;
      StrView name = mods.type == XCB_XKB_SA_TYPE_SET_MODS     ? "SetMods"
                     : mods.type == XCB_XKB_SA_TYPE_LATCH_MODS ? "LatchMods"
                                                               : "LockMods";
      Str modifiers = mods.flags & XCB_XKB_SA_USE_MOD_MAP_MODS
                          ? "modMapMods"
                          : ModMaskText(keymap, mods.realMods | (U32)mods.vmodsLow << 8 |
                                                    (U32)mods.vmodsHigh << 16);
      if (mods.type == XCB_XKB_SA_TYPE_LOCK_MODS) {
        return f("{}(modifiers={}{})", name, modifiers, LockAffectText(mods.flags, false));
      }
      return f("{}(modifiers={}{}{})", name, modifiers,
               mods.flags & XCB_XKB_SA_CLEAR_LOCKS ? ",clearLocks" : "",
               mods.flags & XCB_XKB_SA_LATCH_TO_LOCK ? ",latchToLock" : "");
    }
    case XCB_XKB_SA_TYPE_SET_GROUP:
    case XCB_XKB_SA_TYPE_LATCH_GROUP:
    case XCB_XKB_SA_TYPE_LOCK_GROUP: {
      const xcb_xkb_sa_set_group_t& group = action.setgroup;
      StrView name = group.type == XCB_XKB_SA_TYPE_SET_GROUP     ? "SetGroup"
                     : group.type == XCB_XKB_SA_TYPE_LATCH_GROUP ? "LatchGroup"
                                                                 : "LockGroup";
      bool absolute = group.flags & XCB_XKB_SA_GROUP_ABSOLUTE;
      Str value = absolute ? f("{}", group.group + 1)
                           : f("{}{}", group.group > 0 ? "+" : "", (int)group.group);
      if (group.type == XCB_XKB_SA_TYPE_LOCK_GROUP) return f("{}(group={})", name, value);
      return f("{}(group={}{}{})", name, value,
               group.flags & XCB_XKB_SA_CLEAR_LOCKS ? ",clearLocks" : "",
               group.flags & XCB_XKB_SA_LATCH_TO_LOCK ? ",latchToLock" : "");
    }
    case XCB_XKB_SA_TYPE_MOVE_PTR: {
      const xcb_xkb_sa_move_ptr_t& move = action.moveptr;
      int x = (I16)(move.xLow | (U16)move.xHigh << 8);
      int y = (I16)(move.yLow | (U16)move.yHigh << 8);
      return f("MovePtr(x={}{},y={}{}{})",
               SignText(move.flags & XCB_XKB_SA_MOVE_PTR_FLAG_MOVE_ABSOLUTE_X, x), x,
               SignText(move.flags & XCB_XKB_SA_MOVE_PTR_FLAG_MOVE_ABSOLUTE_Y, y), y,
               move.flags & XCB_XKB_SA_MOVE_PTR_FLAG_NO_ACCELERATION ? ",!accel" : "");
    }
    case XCB_XKB_SA_TYPE_PTR_BTN: {
      const xcb_xkb_sa_ptr_btn_t& button = action.ptrbtn;
      Str text = "PtrBtn(button=";
      text += button.button > 0 && button.button <= 5 ? f("{}", (int)button.button) : "default";
      if (button.count) text += f(",count={}", (int)button.count);
      return text + ")";
    }
    case XCB_XKB_SA_TYPE_LOCK_PTR_BTN: {
      const xcb_xkb_sa_lock_ptr_btn_t& button = action.lockptrbtn;
      Str text = "LockPtrBtn(button=";
      text += button.button > 0 && button.button <= 5 ? f("{}", (int)button.button) : "default";
      return f("{}{})", text, LockAffectText(button.flags, true));
    }
    case XCB_XKB_SA_TYPE_SET_PTR_DFLT: {
      const xcb_xkb_sa_set_ptr_dflt_t& dflt = action.setptrdflt;
      return f("SetPtrDflt(affect=button,button={}{})",
               SignText(dflt.flags & XCB_XKB_SA_SET_PTR_DFLT_FLAG_DFLT_BTN_ABSOLUTE, dflt.value),
               (int)dflt.value);
    }
    case XCB_XKB_SA_TYPE_TERMINATE:
      return "Terminate()";
    case XCB_XKB_SA_TYPE_SWITCH_SCREEN: {
      const xcb_xkb_sa_switch_screen_t& screen = action.switchscreen;
      return f("SwitchScreen(screen={}{},{}same)",
               SignText(screen.flags & XCB_XKB_SWITCH_SCREEN_FLAG_ABSOLUTE, screen.newScreen),
               (int)screen.newScreen,
               screen.flags & XCB_XKB_SWITCH_SCREEN_FLAG_APPLICATION ? "!" : "");
    }
    case XCB_XKB_SA_TYPE_SET_CONTROLS:
    case XCB_XKB_SA_TYPE_LOCK_CONTROLS: {
      const xcb_xkb_sa_set_controls_t& controls = action.setcontrols;
      U32 mask = (controls.boolCtrlsLow | (U32)controls.boolCtrlsHigh << 8) & kNamedControls;
      return f("{}(controls={})",
               controls.type == XCB_XKB_SA_TYPE_SET_CONTROLS ? "SetControls" : "LockControls",
               ControlMaskText(mask));
    }
    case XCB_XKB_SA_TYPE_NO_ACTION:
    case XCB_XKB_SA_TYPE_ISO_LOCK:
    case XCB_XKB_SA_TYPE_ACTION_MESSAGE:
    case XCB_XKB_SA_TYPE_REDIRECT_KEY:
    case XCB_XKB_SA_TYPE_DEVICE_BTN:
    case XCB_XKB_SA_TYPE_LOCK_DEVICE_BTN:
    case XCB_XKB_SA_TYPE_DEVICE_VALUATOR:
      return "NoAction()";
  }
  Str text = f("Private(type=0x{:02x}", action.type);
  for (int i = 0; i < 7; ++i) {
    text += f(",data[{}]=0x{:02x}", i, action.noaction.pad0[i]);
  }
  return text + ")";
}

static void WriteVirtualModifiers(const ServerKeymap& keymap, Str& text) {
  Str names;
  for (int i = kRealModCount; i < keymap.mod_count; ++i) {
    if (!keymap.mod_is_virtual[i]) continue;
    if (!names.empty()) names += ',';
    names += keymap.mod_names[i];
  }
  if (!names.empty()) text += f("\tvirtual_modifiers {};\n\n", names);
}

static void WriteKeycodes(const ServerKeymap& keymap, Str& text) {
  WriteSectionHeader(keymap, text, "xkb_keycodes", keymap.name_lists.keycodesName);
  text += f("\tminimum = {};\n", std::min<int>(keymap.map->minKeyCode, 8));
  text += f("\tmaximum = {};\n", std::max<int>(keymap.map->maxKeyCode, 255));
  for (int kc = keymap.map->minKeyCode; kc <= keymap.map->maxKeyCode; ++kc) {
    const char* name = keymap.keys[kc].name;
    if (!name || name[0] == '\0') continue;
    text += f("\t{:<20} = {};\n", KeyNameText(name), kc);
  }
  for (int i = 0; i < keymap.indicator_count; ++i) {
    if (keymap.indicator_names[i] == XCB_ATOM_NONE) continue;
    text += f("\tindicator {} = \"{}\";\n", i + 1, AtomText(keymap, keymap.indicator_names[i]));
  }
  for (int i = 0; i < keymap.names->nKeyAliases; ++i) {
    const xcb_xkb_key_alias_t& alias = keymap.name_lists.keyAliases[i];
    text += f("\talias {:<14} = {};\n", KeyNameText(alias.alias), KeyNameText(alias.real));
  }
  text += "};\n\n";
}

static void WriteTypes(const ServerKeymap& keymap, Str& text) {
  WriteSectionHeader(keymap, text, "xkb_types", keymap.name_lists.typesName);
  WriteVirtualModifiers(keymap, text);
  for (const KeyType& type : keymap.types) {
    text += f("\ttype \"{}\" {{\n", AtomText(keymap, type.name));
    text += f("\t\tmodifiers= {};\n",
              ModMaskText(keymap, type.wire->mods_mods | (U32)type.wire->mods_vmods << 8));
    xcb_xkb_kt_map_entry_t* entries = xcb_xkb_key_type_map(type.wire);
    xcb_xkb_mod_def_t* preserves =
        type.wire->hasPreserve ? xcb_xkb_key_type_preserve(type.wire) : nullptr;
    for (int i = 0; i < type.wire->nMapEntries; ++i) {
      const xcb_xkb_kt_map_entry_t& entry = entries[i];
      U32 preserve = preserves ? preserves[i].realMods | (U32)preserves[i].vmods << 8 : 0;
      if (entry.level == 0 && preserve == 0) continue;
      Str mods = ModMaskText(keymap, entry.mods_mods | (U32)entry.mods_vmods << 8);
      text += f("\t\tmap[{}]= {};\n", mods, entry.level + 1);
      if (preserve) text += f("\t\tpreserve[{}]= {};\n", mods, ModMaskText(keymap, preserve));
    }
    for (int level = 0; level < type.wire->numLevels; ++level) {
      if (type.level_names[level] == XCB_ATOM_NONE) continue;
      text +=
          f("\t\tlevel_name[{}]= \"{}\";\n", level + 1, AtomText(keymap, type.level_names[level]));
    }
    text += "\t};\n";
  }
  text += "};\n\n";
}

static void WriteIndicatorMap(const ServerKeymap& keymap, Str& text, xcb_atom_t name,
                              const xcb_xkb_indicator_map_t& map) {
  U8 which_groups = StateComponentsFromWire(map.whichGroups);
  U8 which_mods = StateComponentsFromWire(map.whichMods);
  U32 mods = map.realMods | (U32)map.vmods << 8;
  U32 controls = map.ctrls & kNamedControls;
  if (!which_groups && !map.groups && !which_mods && !mods && !controls) return;
  text += f("\tindicator \"{}\" {{\n", AtomText(keymap, name));
  if (which_groups) {
    if (which_groups != kEffectiveStateComponent) {
      text += f("\t\twhichGroupState= {};\n", StateComponentsText(which_groups));
    }
    text += f("\t\tgroups= 0x{:02x};\n", map.groups);
  }
  if (which_mods) {
    if (which_mods != kEffectiveStateComponent) {
      text += f("\t\twhichModState= {};\n", StateComponentsText(which_mods));
    }
    text += f("\t\tmodifiers= {};\n", ModMaskText(keymap, mods));
  }
  if (controls) text += f("\t\tcontrols= {};\n", ControlMaskText(controls));
  text += "\t};\n";
}

static void WriteCompatibility(const ServerKeymap& keymap, Str& text) {
  WriteSectionHeader(keymap, text, "xkb_compatibility", keymap.name_lists.compatName);
  WriteVirtualModifiers(keymap, text);
  text += "\tinterpret.useModMapMods= AnyLevel;\n";
  text += "\tinterpret.repeat= False;\n";
  xcb_xkb_sym_interpret_t* interprets = xcb_xkb_get_compat_map_si_rtrn(keymap.compat);
  for (int i = 0; i < keymap.compat->nSIRtrn; ++i) {
    const xcb_xkb_sym_interpret_t& interpret = interprets[i];
    text += f("\tinterpret {}+{}({}) {{\n", interpret.sym ? KeysymText(interpret.sym) : "Any",
              kMatchNames[interpret.match & XCB_XKB_SYM_INTERP_MATCH_OP_MASK],
              ModMaskText(keymap, interpret.mods));
    if (interpret.virtualMod != kNoVirtualMod) {
      text +=
          f("\t\tvirtualModifier= {};\n", keymap.mod_names[kRealModCount + interpret.virtualMod]);
    }
    if (interpret.match & XCB_XKB_SYM_INTERP_MATCH_LEVEL_ONE_ONLY) {
      text += "\t\tuseModMapMods=level1;\n";
    }
    if (interpret.flags & 0x01) text += "\t\trepeat= True;\n";
    text += f("\t\taction= {};\n", ActionText(keymap, *(const xcb_xkb_action_t*)&interpret.action));
    text += "\t};\n";
  }
  for (int i = 0; i < keymap.indicator_count; ++i) {
    if (!keymap.indicator_maps[i]) continue;
    WriteIndicatorMap(keymap, text, keymap.indicator_names[i], *keymap.indicator_maps[i]);
  }
  text += "};\n\n";
}

static void WriteKeysyms(const Key& key, int group, int level_count, int padding, Str& text) {
  const xcb_xkb_key_sym_map_t& sym_map = *key.sym_map;
  xcb_keysym_t* syms = xcb_xkb_key_sym_map_syms(key.sym_map);
  for (int level = 0; level < level_count; ++level) {
    if (level != 0) text += ", ";
    xkb_keysym_t sym = level < sym_map.width ? syms[group * sym_map.width + level] : 0;
    text += f("{:>{}}", KeysymText(sym), padding);
  }
}

static void WriteActions(const ServerKeymap& keymap, const Key& key, int group, int level_count,
                         Str& text) {
  const xcb_xkb_key_sym_map_t& sym_map = *key.sym_map;
  for (int level = 0; level < level_count; ++level) {
    if (level != 0) text += ", ";
    Str action = key.actions && level < sym_map.width
                     ? ActionText(keymap, key.actions[group * sym_map.width + level])
                     : "NoAction()";
    text += f("{:>30}", action);
  }
}

static void WriteKey(const ServerKeymap& keymap, const Key& key, Str& text) {
  const xcb_xkb_key_sym_map_t& sym_map = *key.sym_map;
  int group_count = sym_map.groupInfo & 0x0f;
  text += f("\tkey {:<20} {{", KeyNameText(key.name));

  bool simple = true;
  bool explicit_types = false;
  bool multi_type = false;
  for (int group = 0; group < group_count; ++group) {
    if (key.explicit_components & (XCB_XKB_EXPLICIT_KEY_TYPE_1 << group)) explicit_types = true;
    if (group != 0 && sym_map.kt_index[group] != sym_map.kt_index[0]) multi_type = true;
  }
  if (explicit_types) {
    simple = false;
    if (multi_type) {
      for (int group = 0; group < group_count; ++group) {
        if (!(key.explicit_components & (XCB_XKB_EXPLICIT_KEY_TYPE_1 << group))) continue;
        text += f("\n\t\ttype[Group{}]= \"{}\",", group + 1,
                  AtomText(keymap, keymap.types[sym_map.kt_index[group]].name));
      }
    } else {
      text += f("\n\t\ttype= \"{}\",", AtomText(keymap, keymap.types[sym_map.kt_index[0]].name));
    }
  }

  bool show_actions = key.explicit_components & XCB_XKB_EXPLICIT_INTERPRET;
  if ((key.explicit_components & XCB_XKB_EXPLICIT_AUTO_REPEAT) || show_actions) {
    text += key.repeats ? "\n\t\trepeat= Yes," : "\n\t\trepeat= No,";
    simple = false;
  }
  if (key.virtual_mods &&
      ((key.explicit_components & XCB_XKB_EXPLICIT_V_MOD_MAP) || show_actions)) {
    text += f("\n\t\tvirtualMods= {},", ModMaskText(keymap, (U32)key.virtual_mods << 8));
  }
  if (sym_map.groupInfo & XCB_XKB_GROUPS_WRAP_CLAMP_INTO_RANGE) {
    text += "\n\t\tgroupsClamp,";
  } else if (sym_map.groupInfo & XCB_XKB_GROUPS_WRAP_REDIRECT_INTO_RANGE) {
    text += f("\n\t\tgroupsRedirect= Group{},", ((sym_map.groupInfo & 0x30) >> 4) + 1);
  }
  if (group_count > 1 || show_actions) simple = false;

  if (simple) {
    text += "\t[ ";
    WriteKeysyms(key, 0, keymap.types[sym_map.kt_index[0]].wire->numLevels, 15, text);
    text += " ] };\n";
    return;
  }
  for (int group = 0; group < group_count; ++group) {
    int level_count = keymap.types[sym_map.kt_index[group]].wire->numLevels;
    if (group != 0) text += ",";
    text += f("\n\t\tsymbols[Group{}]= [ ", group + 1);
    WriteKeysyms(key, group, level_count, show_actions ? 30 : 15, text);
    text += " ]";
    if (show_actions) {
      text += f(",\n\t\tactions[Group{}]= [ ", group + 1);
      WriteActions(keymap, key, group, level_count, text);
      text += " ]";
    }
  }
  text += "\n\t};\n";
}

static void WriteSymbols(const ServerKeymap& keymap, Str& text) {
  WriteSectionHeader(keymap, text, "xkb_symbols", keymap.name_lists.symbolsName);
  int group_names = std::popcount(keymap.names->groupNames);
  for (int i = 0; i < group_names; ++i) {
    if (keymap.name_lists.groups[i] == XCB_ATOM_NONE) continue;
    text += f("\tname[Group{}]=\"{}\";\n", i + 1, AtomText(keymap, keymap.name_lists.groups[i]));
  }
  if (keymap.names->groupNames) text += "\n";

  for (int kc = keymap.map->minKeyCode; kc <= keymap.map->maxKeyCode; ++kc) {
    const Key& key = keymap.keys[kc];
    if (key.sym_map && (key.sym_map->groupInfo & 0x0f)) WriteKey(keymap, key, text);
  }

  for (int mod = 0; mod < kRealModCount; ++mod) {
    Str key_names;
    for (int kc = keymap.map->minKeyCode; kc <= keymap.map->maxKeyCode; ++kc) {
      const Key& key = keymap.keys[kc];
      if (!(key.real_mods & (1u << mod))) continue;
      if (!key_names.empty()) key_names += ", ";
      key_names += KeyNameText(key.name);
    }
    if (!key_names.empty()) {
      text += f("\tmodifier_map {} {{ {} }};\n", keymap.mod_names[mod], key_names);
    }
  }
  text += "};\n\n";
}

Str GetKeymapText() {
  const xcb_query_extension_reply_t* xkb = xcb_get_extension_data(connection, &xcb_xkb_id);
  if (!xkb || !xkb->present) return {};

  xcb_xkb_device_spec_t device = XCB_XKB_ID_USE_CORE_KBD;
  auto map_cookie = xcb_xkb_get_map(connection, device, kMapParts, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0);
  auto names_cookie = xcb_xkb_get_names(connection, device, kNameDetails);
  auto compat_cookie = xcb_xkb_get_compat_map(connection, device, 0, true, 0, 0);
  auto indicators_cookie = xcb_xkb_get_indicator_map(connection, device, 0xffffffff);
  auto controls_cookie = xcb_xkb_get_controls(connection, device);

  std::unique_ptr<xcb_xkb_get_map_reply_t, crt::DeleteWithFree> map(
      xcb_xkb_get_map_reply(connection, map_cookie, nullptr));
  std::unique_ptr<xcb_xkb_get_names_reply_t, crt::DeleteWithFree> names(
      xcb_xkb_get_names_reply(connection, names_cookie, nullptr));
  std::unique_ptr<xcb_xkb_get_compat_map_reply_t, crt::DeleteWithFree> compat(
      xcb_xkb_get_compat_map_reply(connection, compat_cookie, nullptr));
  std::unique_ptr<xcb_xkb_get_indicator_map_reply_t, crt::DeleteWithFree> indicators(
      xcb_xkb_get_indicator_map_reply(connection, indicators_cookie, nullptr));
  std::unique_ptr<xcb_xkb_get_controls_reply_t, crt::DeleteWithFree> controls(
      xcb_xkb_get_controls_reply(connection, controls_cookie, nullptr));
  if (!map || !names || !compat || !indicators || !controls) return {};

  ServerKeymap keymap{.map = map.get(),
                      .names = names.get(),
                      .compat = compat.get(),
                      .indicators = indicators.get(),
                      .controls = controls.get()};
  if (!ReadMap(keymap) || !ReadNames(keymap) || !ReadAtomNames(keymap) ||
      !InterpretMatchesAndModifiersAreKnown(keymap)) {
    return {};
  }

  Str text = "xkb_keymap {\n";
  WriteKeycodes(keymap, text);
  WriteTypes(keymap, text);
  WriteCompatibility(keymap, text);
  WriteSymbols(keymap, text);
  text += "};\n";
  return text;
}

}  // namespace xcb
