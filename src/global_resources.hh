// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkMesh.h>
#include <include/core/SkRefCnt.h>

#include <memory>
#include <vector>

namespace automat::resources {

extern std::vector<std::unique_ptr<sk_sp<SkMeshSpecification>>> mesh_specifications;

// Make a copy of the given sk_sp and return a reference to it.
//
// The sk_sp will be released when `Release()` is called.
//
// This can be used to cache an expensive-to-compute resource.
sk_sp<SkMeshSpecification>& Hold(sk_sp<SkMeshSpecification>);

void Release();

}  // namespace automat::resources
