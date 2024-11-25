// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "global_resources.hh"

namespace automat::resources {

std::vector<std::unique_ptr<sk_sp<SkMeshSpecification>>> mesh_specifications;
std::vector<std::unique_ptr<sk_sp<SkRefCnt>>> sk_ref_cnt_objects;

template <>
sk_sp<SkMeshSpecification>& Hold(sk_sp<SkMeshSpecification> spec) {
  mesh_specifications.emplace_back(std::make_unique<sk_sp<SkMeshSpecification>>(spec));
  return *mesh_specifications.back();
}

template <>
sk_sp<SkShader>& Hold(sk_sp<SkShader> shader) {
  sk_ref_cnt_objects.emplace_back(std::make_unique<sk_sp<SkRefCnt>>(shader));
  return *reinterpret_cast<sk_sp<SkShader>*>(sk_ref_cnt_objects.back().get());
}

void Release() {
  mesh_specifications.clear();
  sk_ref_cnt_objects.clear();
}

}  // namespace automat::resources
