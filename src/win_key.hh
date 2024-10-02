// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "keyboard.hh"

automat::gui::AnsiKey ScanCodeToKey(uint32_t scan_code);
automat::gui::AnsiKey VirtualKeyToKey(uint8_t virtual_key);
uint32_t KeyToScanCode(automat::gui::AnsiKey key);
uint8_t KeyToVirtualKey(automat::gui::AnsiKey key);
