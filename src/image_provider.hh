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

    Table(StrView name) : Interface::Table(Interface::kImageProvider, name) {}
  };

  struct State {};

  INTERFACE_BOUND(ImageProvider, Interface)

  sk_sp<SkImage> GetImage() const {
    return table->get_image ? table->get_image(*this) : nullptr;
  }

  // ImplT must provide:
  //   using Parent = SomeObject;
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   sk_sp<SkImage> GetImage();
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = ImageProvider;

    static Table& GetTable() {
      static Table tbl = []{
        Table t(ImplT::kName);
        t.get_image = +[](ImageProvider self) -> sk_sp<SkImage> {
          return static_cast<ImplT&>(self).GetImage();
        };
        t.state_off = ImplT::Offset();
        return t;
      }();
      return tbl;
    }
  };
};

}  // namespace automat
