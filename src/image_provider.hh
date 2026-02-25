// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>

#include "interface.hh"

namespace automat {

struct Object;

// Interface for objects that can provide image data
struct ImageProvider : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kImageProvider; }

    // Function pointer for getting the image.
    sk_sp<SkImage> (*get_image)(ImageProvider) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kImageProvider, name) {}
  };

  struct State {};

  INTERFACE_BOUND(ImageProvider, Interface)

  sk_sp<SkImage> GetImage() const { return table->get_image ? table->get_image(*this) : nullptr; }

  // ImplT must provide:
  //   using Parent = SomeObject;
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   sk_sp<SkImage> GetImage();
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = ImageProvider;

    template <typename T>
    static sk_sp<SkImage> InvokeGetImage(ImageProvider self) {
      return static_cast<T&>(self).GetImage();
    }

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.state_off = ImplT::Offset();
      t.get_image = &InvokeGetImage<ImplT>;
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

}  // namespace automat
