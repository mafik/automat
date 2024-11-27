// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "global_resources.hh"

#include "log.hh"

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

std::vector<RuntimeEffectBuilder*> runtime_effect_builders;

RuntimeEffectBuilder::RuntimeEffectBuilder(maf::StrView sksl) {
  auto fs = SkString(sksl);
  auto result = SkRuntimeEffect::MakeForShader(fs);
  if (!result.errorText.isEmpty()) {
    ERROR << "Failed to compile shader: " << result.errorText.c_str();
    return;
  }
  builder.emplace(result.effect);
  runtime_effect_builders.push_back(this);
}

RuntimeEffectBuilder::~RuntimeEffectBuilder() {
  runtime_effect_builders.erase(
      std::remove(runtime_effect_builders.begin(), runtime_effect_builders.end(), this),
      runtime_effect_builders.end());
}

void Release() {
  mesh_specifications.clear();
  sk_ref_cnt_objects.clear();
  for (auto* builder : runtime_effect_builders) {
    builder->builder.reset();
  }
}

}  // namespace automat::resources
