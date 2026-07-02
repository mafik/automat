// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Automat's own XKEYBOARD implementation. It answers the XKB requests a toolkit issues to
// build its keymap (UseExtension, GetMap, GetNames, GetControls, GetCompatMap,
// GetIndicatorMap, GetState, PerClientFlags, GetDeviceInfo) by encoding the shared keymap
// (keymap.hpp) onto the wire, so embedded X11 clients get the current layout no matter what
// windowing system — if any — Automat itself runs on. The keymap is expressed in real
// modifiers only: xkb_keymap_key_get_mods_for_level resolves virtual modifiers (LevelThree,
// NumLock, ...) to the real ones the key event already carries, so no virtual-modifier
// machinery is needed for the client to pick the right shift level.
//
// Wire layouts are from /usr/include/X11/extensions/XKBproto.h (little-endian).

#include "x11_xkb.hpp"

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cstring>

#include "format.hpp"
#include "key.hpp"
#include "keymap.hpp"
#include "vec.hpp"
#include "x11_protocol.hpp"

namespace automat::x11::xkb {

namespace {

constexpr U8 kCoreKbd = 3;  // the one keyboard device Automat presents

// A modifier mask that produces a non-base shift level of a key type.
struct LevelEntry {
  U8 mask;
  U8 level;
};
struct KeyType {
  U8 num_levels;
  Vec<LevelEntry> entries;  // one per non-base level reachable by a modifier combination
};

xkb_keymap* Km() { return keymap ? keymap->xkb : nullptr; }

// Core protocol keycodes are CARD8: clamp the keymap's range to 8..255 (the compiled
// default keymap extends to 708; those keys are unreachable over X11).
U8 MinKeycode(xkb_keymap* km) {
  return km ? (U8)std::clamp<xkb_keycode_t>(xkb_keymap_min_keycode(km), 8, 255) : 8;
}
U8 MaxKeycode(xkb_keymap* km) {
  return km ? (U8)std::clamp<xkb_keycode_t>(xkb_keymap_max_keycode(km), 8, 255) : 8;
}

U8 NumGroups(xkb_keymap* km) {
  xkb_layout_index_t groups = km ? xkb_keymap_num_layouts(km) : 1;
  return (U8)std::clamp<xkb_layout_index_t>(groups, 1, 4);
}

// The synthesized key-type table plus each key/group's index into it. Built once per keymap
// and shared by GetMap (structure) and GetNames (labels) so the two always agree.
struct TypeTable {
  xkb_keymap* built_for = nullptr;
  Vec<KeyType> types;
  U8 index[256][4] = {};
} type_table;

U8 Intern(TypeTable& tt, const KeyType& t) {
  for (size_t i = 0; i < tt.types.size(); ++i) {
    if (tt.types[i].num_levels != t.num_levels) continue;
    if (tt.types[i].entries.size() != t.entries.size()) continue;
    bool same = true;
    for (size_t j = 0; j < t.entries.size(); ++j)
      if (tt.types[i].entries[j].mask != t.entries[j].mask ||
          tt.types[i].entries[j].level != t.entries[j].level) {
        same = false;
        break;
      }
    if (same) return (U8)i;
  }
  if (tt.types.size() >= 255) return 0;
  tt.types.push_back(t);
  return (U8)(tt.types.size() - 1);
}

void EnsureTypes(xkb_keymap* km) {
  if (type_table.built_for == km) return;
  type_table.built_for = km;
  type_table.types.clear();
  std::memset(type_table.index, 0, sizeof(type_table.index));
  type_table.types.push_back({1, {}});  // index 0 is always ONE_LEVEL
  if (!km) return;
  U8 lo = MinKeycode(km), hi = MaxKeycode(km);
  for (U32 kc = lo; kc <= hi; ++kc) {
    xkb_layout_index_t groups = xkb_keymap_num_layouts_for_key(km, kc);
    for (xkb_layout_index_t g = 0; g < groups && g < 4; ++g) {
      xkb_level_index_t levels = xkb_keymap_num_levels_for_key(km, kc, g);
      if (levels < 1) levels = 1;
      KeyType t;
      t.num_levels = (U8)levels;
      for (xkb_level_index_t level = 1; level < levels; ++level) {
        xkb_mod_mask_t masks[16];
        size_t n = xkb_keymap_key_get_mods_for_level(km, kc, g, level, masks, 16);
        for (size_t i = 0; i < n; ++i)
          if (U8 m = (U8)(masks[i] & 0xff)) t.entries.push_back({m, (U8)level});
      }
      type_table.index[kc][g] = Intern(type_table, t);
    }
  }
}

// A little-endian byte builder for wire sections.
struct Wire {
  Vec<U8> b;
  void W8(U8 v) { b.push_back(v); }
  void W16(U16 v) {
    b.push_back(v & 0xff);
    b.push_back((v >> 8) & 0xff);
  }
  void W32(U32 v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff);
  }
  void Pad(size_t n) { b.insert(b.end(), n, 0); }
  void Align4() {
    if (b.size() & 3) Pad(4 - (b.size() & 3));
  }
  void Append(const Wire& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};

// A reply builder: the 8-byte prologue, then the body; Send() stamps the sequence and
// length and appends the (>=32-byte, 4-aligned) reply to the client.
struct Reply : Wire {
  explicit Reply(U8 second = kCoreKbd) {
    W8(1);       // X_Reply
    W8(second);  // deviceID (or `supported` for UseExtension)
    W16(0);      // sequenceNumber, stamped by Send
    W32(0);      // length, stamped by Send
  }
};

void Send(Client& c, Reply& r) {
  if (r.b.size() < 32) r.Pad(32 - r.b.size());
  r.Align4();
  U16 seq = c.sequence;
  std::memcpy(r.b.data() + 2, &seq, 2);
  U32 len = (U32)((r.b.size() - 32) / 4);
  std::memcpy(r.b.data() + 4, &len, 4);
  c.out.append((const char*)r.b.data(), r.b.size());
}

void UseExtension(Client& c) {
  Reply r(1 /* supported */);
  r.W16(1);  // serverMajor
  r.W16(0);  // serverMinor
  Send(c, r);
}

void GetMap(Client& c, const U8* raw, size_t raw_len) {
  Server& s = c.server;
  xkb_keymap* km = Km();
  EnsureTypes(km);
  U8 lo = MinKeycode(km), hi = MaxKeycode(km);

  // present echoes what the request asked for (full or partial; partial requests get the
  // full range, which the reply header describes, so clients parse it the same). The
  // sections with no data on Automat's synthetic keyboard (actions, behaviors, explicit,
  // virtual modifiers) are echoed with zero counts: xkbcommon-x11 refuses a keymap whose
  // reply lacks the bits it asked for.
  U16 wanted = 0xff;
  if (raw_len >= 10) {
    U16 full, partial;
    std::memcpy(&full, raw + 6, 2);
    std::memcpy(&partial, raw + 8, 2);
    wanted = (full | partial) & 0xff;
  }
  constexpr U16 kKeyTypes = 1 << 0, kKeySyms = 1 << 1, kModMap = 1 << 2;

  Wire types;
  for (auto& t : type_table.types) {
    U8 mask = 0;
    for (auto& e : t.entries) mask |= e.mask;
    types.W8(mask);  // mask
    types.W8(mask);  // realMods
    types.W16(0);    // virtualMods
    types.W8(t.num_levels);
    types.W8((U8)t.entries.size());  // nMapEntries
    types.W8(0);                     // preserve
    types.W8(0);                     // pad
    for (auto& e : t.entries) {
      types.W8(1);        // active
      types.W8(e.mask);   // mask
      types.W8(e.level);  // level
      types.W8(e.mask);   // realMods
      types.W16(0);       // virtualMods
      types.W16(0);       // pad
    }
  }

  Wire syms;
  U32 total_syms = 0;
  for (U32 kc = lo; kc <= hi; ++kc) {
    xkb_layout_index_t groups = km ? xkb_keymap_num_layouts_for_key(km, kc) : 0;
    if (groups > 4) groups = 4;
    U8 width = 0;
    for (xkb_layout_index_t g = 0; g < groups; ++g) {
      xkb_level_index_t levels = xkb_keymap_num_levels_for_key(km, kc, g);
      if (levels > width) width = (U8)levels;
    }
    U16 nsyms = (U16)(groups * width);
    for (int g = 0; g < 4; ++g) syms.W8(g < (int)groups ? type_table.index[kc][g] : 0);
    syms.W8((U8)(groups & 0x0f));  // groupInfo: number of groups
    syms.W8(width);
    syms.W16(nsyms);
    for (xkb_layout_index_t g = 0; g < groups; ++g) {
      xkb_level_index_t levels = xkb_keymap_num_levels_for_key(km, kc, g);
      for (U8 level = 0; level < width; ++level) {
        const xkb_keysym_t* sy = nullptr;
        int n = level < levels ? xkb_keymap_key_get_syms_by_level(km, kc, g, level, &sy) : 0;
        syms.W32(n > 0 ? sy[0] : 0);
      }
    }
    total_syms += nsyms;
  }
  U8 n_keys = (U8)(hi - lo + 1);

  Wire modmap;
  U8 total_modmap = 0;
  for (U32 kc = lo; kc <= hi; ++kc)
    if (s.key_mods[kc]) {
      modmap.W8((U8)kc);
      modmap.W8(s.key_mods[kc]);
      ++total_modmap;
    }
  modmap.Align4();

  bool send_types = wanted & kKeyTypes, send_syms = wanted & kKeySyms,
       send_modmap = wanted & kModMap;
  Reply r;
  r.W16(0);  // pad1
  r.W8(lo);
  r.W8(hi);
  r.W16(wanted);                                       // present
  r.W8(0);                                             // firstType
  r.W8(send_types ? (U8)type_table.types.size() : 0);  // nTypes
  r.W8((U8)type_table.types.size());                   // totalTypes
  r.W8(lo);                                            // firstKeySym
  r.W16(send_syms ? (U16)total_syms : 0);              // totalSyms
  r.W8(send_syms ? n_keys : 0);                        // nKeySyms
  r.W8(0);                                             // firstKeyAct
  r.W16(0);                                            // totalActs
  r.W8(0);                                             // nKeyActs
  r.W8(0);                                             // firstKeyBehavior
  r.W8(0);                                             // nKeyBehaviors
  r.W8(0);                                             // totalKeyBehaviors
  r.W8(0);                                             // firstKeyExplicit
  r.W8(0);                                             // nKeyExplicit
  r.W8(0);                                             // totalKeyExplicit
  r.W8(lo);                                            // firstModMapKey
  r.W8(send_modmap ? n_keys : 0);                      // nModMapKeys
  r.W8(send_modmap ? total_modmap : 0);                // totalModMapKeys
  r.W8(0);                                             // firstVModMapKey
  r.W8(0);                                             // nVModMapKeys
  r.W8(0);                                             // totalVModMapKeys
  r.W8(0);                                             // pad2
  r.W16(0);                                            // virtualMods
  if (send_types) r.Append(types);
  if (send_syms) r.Append(syms);
  if (send_modmap) r.Append(modmap);
  Send(c, r);
}

void GetNames(Client& c, const U8* raw, size_t raw_len) {
  Server& s = c.server;
  xkb_keymap* km = Km();
  EnsureTypes(km);
  U8 lo = MinKeycode(km), hi = MaxKeycode(km);
  U8 n_keys = (U8)(hi - lo + 1);
  U8 n_types = (U8)type_table.types.size();
  U8 groups = NumGroups(km);

  U16 nkt_levels = 0;
  for (auto& t : type_table.types) nkt_levels += t.num_levels;

  auto Atom = [&](StrView name) -> U32 { return s.InternAtom(name, false); };

  constexpr U32 kKeycodes = 1u << 0, kGeometry = 1u << 1, kSymbols = 1u << 2,
                kPhysSymbols = 1u << 3, kTypes = 1u << 4, kCompat = 1u << 5;
  constexpr U32 kKeyTypeNames = 1u << 6, kKTLevelNames = 1u << 7, kIndicatorNames = 1u << 8;
  constexpr U32 kKeyNames = 1u << 9, kKeyAliases = 1u << 10, kVirtualModNames = 1u << 11;
  constexpr U32 kGroupNames = 1u << 12, kRGNames = 1u << 13;
  // Everything a name can be reported for; the empty sets (indicators, aliases, virtual
  // modifiers, radio groups) contribute no bytes but their bits must still be echoed —
  // xkbcommon-x11 refuses the keymap when the virtual-modifier-names bit is missing.
  constexpr U32 kSupported = kKeycodes | kGeometry | kSymbols | kPhysSymbols | kTypes | kCompat |
                             kKeyTypeNames | kKTLevelNames | kIndicatorNames | kKeyNames |
                             kKeyAliases | kVirtualModNames | kGroupNames | kRGNames;
  U32 which = 0x3fff;
  if (raw_len >= 12) std::memcpy(&which, raw + 8, 4);
  which &= kSupported;

  Reply r;
  r.W32(which);
  r.W8(lo);                        // minKeyCode
  r.W8(hi);                        // maxKeyCode
  r.W8(n_types);                   // nTypes
  r.W8((U8)((1u << groups) - 1));  // groupNames: which groups are named
  r.W16(0);                        // virtualMods: none defined, so no names follow
  r.W8(lo);                        // firstKey
  r.W8(n_keys);                    // nKeys
  r.W32(0);                        // indicators
  r.W8(0);                         // nRadioGroups
  r.W8(0);                         // nKeyAliases
  r.W16(nkt_levels);               // nKTLevels
  r.W32(0);                        // pad3

  // The value list, in the wire order fixed by the XKB protocol (which is not the bit
  // order: virtual-modifier and group names precede key names).
  if (which & kKeycodes) r.W32(Atom("evdev"));
  if (which & kGeometry) r.W32(0);  // None
  if (which & kSymbols) r.W32(Atom("automat"));
  if (which & kPhysSymbols) r.W32(0);  // None
  if (which & kTypes) r.W32(Atom("automat"));
  if (which & kCompat) r.W32(Atom("automat"));
  if (which & kKeyTypeNames) {
    for (auto& t : type_table.types) {
      Str name = t.num_levels == 1   ? "ONE_LEVEL"
                 : t.num_levels == 2 ? "TWO_LEVEL"
                 : t.num_levels == 3 ? "THREE_LEVEL"
                 : t.num_levels == 4 ? "FOUR_LEVEL"
                                     : f("TYPE_%d", t.num_levels);
      r.W32(Atom(name));
    }
  }
  if (which & kKTLevelNames) {
    for (auto& t : type_table.types) r.W8(t.num_levels);  // levels per type
    r.Align4();
    for (auto& t : type_table.types)
      for (U8 level = 0; level < t.num_levels; ++level) r.W32(Atom(f("Level%d", level + 1)));
  }
  if (which & kGroupNames) {
    for (U8 g = 0; g < groups; ++g) {
      const char* n = km ? xkb_keymap_layout_get_name(km, g) : nullptr;
      r.W32(Atom(n ? n : f("Group %d", g + 1)));
    }
  }
  if (which & kKeyNames) {
    for (U32 kc = lo; kc <= hi; ++kc) {  // 4 raw bytes each
      const char* n = km ? xkb_keymap_key_get_name(km, kc) : nullptr;
      char name[4] = {0, 0, 0, 0};
      if (n)
        for (int i = 0; i < 4 && n[i]; ++i) name[i] = n[i];
      r.b.insert(r.b.end(), name, name + 4);
    }
  }
  Send(c, r);
}

void GetControls(Client& c) {
  xkb_keymap* km = Km();
  Reply r;
  r.W8(1);              // mkDfltBtn
  r.W8(NumGroups(km));  // numGroups
  r.W8(0);              // groupsWrap (XkbWrapIntoRange)
  r.W8(0);              // internalMods
  r.W8(0);              // ignoreLockMods
  r.W8(0);              // internalRealMods
  r.W8(0);              // ignoreLockRealMods
  r.W8(0);              // pad1
  r.W16(0);             // internalVMods
  r.W16(0);             // ignoreLockVMods
  r.W16(660);           // repeatDelay
  r.W16(40);            // repeatInterval
  r.W16(0);             // slowKeysDelay
  r.W16(0);             // debounceDelay
  r.W16(300);           // mkDelay
  r.W16(100);           // mkInterval
  r.W16(500);           // mkTimeToMax
  r.W16(500);           // mkMaxSpeed
  r.W16(0);             // mkCurve
  r.W16(0);             // axOptions
  r.W16(0);             // axTimeout
  r.W16(0);             // axtOptsMask
  r.W16(0);             // axtOptsValues
  r.W16(0);             // pad2
  r.W32(0);             // axtCtrlsMask
  r.W32(0);             // axtCtrlsValues
  r.W32(1);             // enabledCtrls = XkbRepeatKeysMask
  // perKeyRepeat: a 256-bit array, bit k = key k repeats.
  U8 per_key[32] = {};
  if (km)
    for (U32 kc = MinKeycode(km); kc <= MaxKeycode(km); ++kc)
      if (xkb_keymap_key_repeats(km, kc)) per_key[kc >> 3] |= (U8)(1u << (kc & 7));
  r.b.insert(r.b.end(), per_key, per_key + 32);
  Send(c, r);
}

void GetState(Client& c) {
  Reply r;  // 32-byte fixed reply, all state zero (clients track group from key events)
  Send(c, r);
}

void GetCompatMap(Client& c, const U8* raw, size_t raw_len) {
  // One symbol interpretation — the catch-all every real server ends its table with:
  // "Any+AnyOf(all) { action = SetMods(modifiers=modMapMods, clearLocks); }". It restates
  // what the modifier map already says (a key sets the real modifiers bound to it), and a
  // compat map with no interpretations at all is rejected by Xlib consumers. The group
  // compat entries the request asks for are emitted as no-modifier records.
  U8 groups = raw_len >= 7 ? raw[6] : 0;
  groups &= (U8)((1u << NumGroups(Km())) - 1);
  U16 first_si = 0, n_si = 1;
  if (raw_len >= 12 && !raw[7] /* getAllSI */) {
    std::memcpy(&first_si, raw + 8, 2);
    std::memcpy(&n_si, raw + 10, 2);
    if (first_si > 0) first_si = 1;
    if (n_si > 1u - first_si) n_si = 1 - first_si;
  }
  Reply r;
  r.W8(groups);     // groups whose compat maps follow
  r.W8(0);          // pad1
  r.W16(first_si);  // firstSI
  r.W16(n_si);      // nSI
  r.W16(1);         // nTotalSI
  r.W32(0);
  r.W32(0);
  r.W32(0);
  r.W32(0);
  if (n_si) {
    r.W32(0);           // sym = NoSymbol (any)
    r.W8(0xff);         // mods: any
    r.W8(2);            // match = XkbSI_AnyOf
    r.W8(0xff);         // virtualMod = XkbNoModifier
    r.W8(0);            // flags
    r.W8(0x01);         // act.type = XkbSA_SetMods
    r.W8(0x01 | 0x04);  // act flags = XkbSA_ClearLocks | XkbSA_UseModMapMods
    r.Pad(6);           // act mask/realMods/vmods unused with UseModMapMods
  }
  for (int g = 0; g < 4; ++g)
    if (groups & (1u << g)) r.W32(0);  // xkbModsWireDesc{mask=0, realMods=0, virtualMods=0}
  Send(c, r);
}

void GetGeometry(Client& c) {
  Reply r;   // found=False: Automat defines no keyboard geometry, and clients accept that
  r.W32(0);  // name
  r.W8(0);   // found
  Send(c, r);
}

void GetKbdByName(Client& c) {
  xkb_keymap* km = Km();
  Reply r;  // loaded=False, nothing reported: the client falls back to piecewise Get* calls
  r.W8(MinKeycode(km));  // minKeyCode
  r.W8(MaxKeycode(km));  // maxKeyCode
  r.W8(0);               // loaded
  r.W8(0);               // newKeyboard
  r.W16(0);              // found
  r.W16(0);              // reported
  Send(c, r);
}

void GetIndicatorMap(Client& c) {
  Reply r;   // no indicators
  r.W32(0);  // which
  r.W32(0);  // realIndicators
  r.W8(0);   // nIndicators
  Send(c, r);
}

void PerClientFlags(Client& c, const U8* raw, size_t raw_len) {
  U32 change = 0, value = 0;
  if (raw_len >= 14) {
    std::memcpy(&change, raw + 6, 4);
    std::memcpy(&value, raw + 10, 4);
  }
  Reply r;
  r.W32(0x1f);            // supported = XkbPCF_AllFlagsMask
  r.W32(change & value);  // value: honor whatever the client asked to set
  r.W32(0);               // autoCtrls
  r.W32(0);               // autoCtrlValues
  Send(c, r);
}

void GetDeviceInfo(Client& c) {
  Reply r;   // minimal: the client only needs a device id back
  r.W16(0);  // present
  r.W16(0);  // supported
  r.W16(0);  // unsupported
  r.W16(0);  // nDeviceLedFBs
  r.W8(0);   // firstBtnWanted
  r.W8(0);   // nBtnsWanted
  r.W8(0);   // firstBtnRtrn
  r.W8(0);   // nBtnsRtrn
  r.W8(0);   // totalBtns
  r.W8(0);   // hasOwnState
  r.W16(0);  // dfltKbdFB
  r.W16(0);  // dfltLedFB
  r.W16(0);  // pad
  r.W32(0);  // devType
  r.W16(0);  // the counted device-name string: zero length
  Send(c, r);
}

}  // namespace

void FillModifiers(ui::Key& key, U32 mask) {
  key.shift = mask & 0x01;
  key.caps_lock = mask & 0x02;
  key.ctrl = mask & 0x04;
  key.alt = mask & 0x08;
  key.num_lock = mask & 0x10;
  key.level5 = mask & 0x20;
  key.windows = mask & 0x40;
  key.alt_gr = mask & 0x80;
}

U8 ModifierMask(const ui::Key& key) {
  return (key.shift ? 0x01 : 0) | (key.caps_lock ? 0x02 : 0) | (key.ctrl ? 0x04 : 0) |
         (key.alt ? 0x08 : 0) | (key.num_lock ? 0x10 : 0) | (key.level5 ? 0x20 : 0) |
         (key.windows ? 0x40 : 0) | (key.alt_gr ? 0x80 : 0);
}

bool Dispatch(Client& c, U8 minor, const U8* raw, size_t raw_len) {
  switch (minor) {
    case 0:
      UseExtension(c);
      return true;
    case 4:
      GetState(c);
      return true;
    case 6:
      GetControls(c);
      return true;
    case 8:
      GetMap(c, raw, raw_len);
      return true;
    case 10:
      GetCompatMap(c, raw, raw_len);
      return true;
    case 13:
      GetIndicatorMap(c);
      return true;
    case 17:
      GetNames(c, raw, raw_len);
      return true;
    case 19:
      GetGeometry(c);
      return true;
    case 21:
      PerClientFlags(c, raw, raw_len);
      return true;
    case 23:
      GetKbdByName(c);
      return true;
    case 24:
      GetDeviceInfo(c);
      return true;
    case 1:         // SelectEvents
    case 3:         // Bell
    case 5:         // LatchLockState
    case 7:         // SetControls
    case 9:         // SetMap
    case 11:        // SetCompatMap
    case 14:        // SetIndicatorMap
    case 16:        // SetNamedIndicator
    case 18:        // SetNames
    case 20:        // SetGeometry
    case 25:        // SetDeviceInfo
      return true;  // no reply; nothing to change on Automat's synthetic keyboard
    default:
      // A reply-bearing request we do not synthesize (GetIndicatorState, ListComponents, ...):
      // answer with an error rather than leaving the client waiting for a reply.
      c.Error(1 /* Request */, 0, minor, kMajorOpcode);
      return true;
  }
}

}  // namespace automat::x11::xkb
