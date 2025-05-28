// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "global_resources.hh"

#include "path.hh"

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

template <>
sk_sp<SkRuntimeEffect>& Hold(sk_sp<SkRuntimeEffect> shader) {
  sk_ref_cnt_objects.emplace_back(std::make_unique<sk_sp<SkRefCnt>>(shader));
  return *reinterpret_cast<sk_sp<SkRuntimeEffect>*>(sk_ref_cnt_objects.back().get());
}

void Release() {
  mesh_specifications.clear();
  sk_ref_cnt_objects.clear();
}

sk_sp<SkRuntimeEffect> CompileShader(fs::VFile sksl_file, Status& status) {
  auto fs = SkString(sksl_file.content);
  SkRuntimeEffect::Options options;
  auto name = Path(sksl_file.path).Stem();
  options.fName = name;
  auto result = SkRuntimeEffect::MakeForShader(fs, options);
  if (!result.errorText.isEmpty()) {
    AppendErrorMessage(status) += result.errorText.c_str();
    return nullptr;
  }
  return Hold(std::move(result.effect));  // creates a copy of sk_sp (it's fine)
}
}  // namespace automat::resources
