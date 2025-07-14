// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "library_mouse.hh"

#include <include/core/SkShader.h>

namespace automat::library::mouse {

PersistentImage base_texture = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_base_webp, PersistentImage::MakeArgs{.scale = kTextureScale});

}  // namespace automat::library::mouse