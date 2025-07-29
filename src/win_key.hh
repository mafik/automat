// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "keyboard.hh"

automat::ui::AnsiKey ScanCodeToKey(uint32_t scan_code);
automat::ui::AnsiKey VirtualKeyToKey(uint8_t virtual_key);
uint32_t KeyToScanCode(automat::ui::AnsiKey key);
uint8_t KeyToVirtualKey(automat::ui::AnsiKey key);
