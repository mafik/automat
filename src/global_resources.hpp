#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkMesh.h>
#include <include/core/SkRefCnt.h>
#include <include/effects/SkRuntimeEffect.h>

#include "status.hpp"
#include "virtual_fs.hpp"

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

template <>
sk_sp<SkRuntimeEffect>& Hold(sk_sp<SkRuntimeEffect>);

sk_sp<SkRuntimeEffect> CompileShader(fs::VFile sksl_file, Status& status);

void Release();

}  // namespace automat::resources
