// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkMesh.h>
#include <include/core/SkRefCnt.h>

#include "str.hh"

namespace automat::resources {

// Make a copy of the given sk_sp and return a reference to it.
//
// The sk_sp will be released when `Release()` is called.
//
// This can be used to cache an expensive-to-compute resource.
template <typename T>
sk_sp<T>& Hold(sk_sp<T>);

template <>
sk_sp<SkMeshSpecification>& Hold(sk_sp<SkMeshSpecification>);

template <>
sk_sp<SkShader>& Hold(sk_sp<SkShader>);

struct RuntimeEffectBuilder {
  std::optional<SkRuntimeEffectBuilder> builder;

  RuntimeEffectBuilder(maf::StrView sksl);
  ~RuntimeEffectBuilder();

  SkRuntimeEffectBuilder* operator->() { return &*builder; }
};

void Release();

}  // namespace automat::resources