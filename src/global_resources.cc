// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "global_resources.hh"

namespace automat::resources {

std::vector<std::unique_ptr<sk_sp<SkMeshSpecification>>> mesh_specifications;

sk_sp<SkMeshSpecification>& Hold(sk_sp<SkMeshSpecification> spec) {
  mesh_specifications.emplace_back(std::make_unique<sk_sp<SkMeshSpecification>>(spec));
  return *mesh_specifications.back();
}

void Release() { mesh_specifications.clear(); }

}  // namespace automat::resources
