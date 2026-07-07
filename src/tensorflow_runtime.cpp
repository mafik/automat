// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "tensorflow_runtime.hpp"

#include <tensorflow/cc/client/client_session.h>
#include <tensorflow/cc/framework/scope.h>
#include <tensorflow/cc/ops/array_ops.h>
#include <tensorflow/core/framework/tensor.h>
#include <tensorflow/core/graph/node_builder.h>

#include <algorithm>

namespace automat::tf {

struct Value {
  tensorflow::Tensor tensor;
};

std::shared_ptr<Value> ImageToValue(const uint8_t* rgba, int width, int height) {
  auto v = std::make_shared<Value>();
  v->tensor =
      tensorflow::Tensor(tensorflow::DT_FLOAT, tensorflow::TensorShape({1, height, width, 3}));
  float* out = v->tensor.flat<float>().data();
  for (size_t i = 0; i < (size_t)width * height; ++i) {
    out[i * 3 + 0] = rgba[i * 4 + 0] / 255.f;
    out[i * 3 + 1] = rgba[i * 4 + 1] / 255.f;
    out[i * 3 + 2] = rgba[i * 4 + 2] / 255.f;
  }
  return v;
}

std::shared_ptr<Value> RunUnaryOp(const std::string& op_type, const Value& input,
                                  std::string& error) {
  using namespace tensorflow;
  Scope scope = Scope::NewRootScope();
  auto placeholder = ops::Placeholder(scope.WithOpName("input"), input.tensor.dtype());
  Node* node = nullptr;
  Status s = NodeBuilder(scope.GetUniqueNameForOp(op_type), op_type)
                 .Input(placeholder.node())
                 .Finalize(scope.graph(), &node);
  if (!s.ok()) {
    error = op_type + ": " + std::string(s.message());
    return nullptr;
  }
  ClientSession session(scope);
  std::vector<Tensor> outputs;
  s = session.Run({{placeholder, input.tensor}}, {Output(node)}, &outputs);
  if (!s.ok()) {
    error = op_type + ": " + std::string(s.message());
    return nullptr;
  }
  if (outputs.empty()) {
    error = op_type + ": produced no output";
    return nullptr;
  }
  auto v = std::make_shared<Value>();
  v->tensor = std::move(outputs[0]);
  return v;
}

Facts Describe(const Value& v) {
  const tensorflow::Tensor& t = v.tensor;
  Facts facts;
  std::string dims;
  for (int i = 0; i < t.dims(); ++i) {
    if (!dims.empty()) dims += ",";
    dims += std::to_string(t.dim_size(i));
  }
  facts.format = "f32[" + dims + "]";
  facts.device = "CPU";
  if (t.dtype() == tensorflow::DT_FLOAT) {
    auto flat = t.flat<float>();
    size_t n = flat.size();
    const float* data = flat.data();
    float min = n ? data[0] : 0, max = n ? data[0] : 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
      min = std::min(min, data[i]);
      max = std::max(max, data[i]);
      sum += data[i];
    }
    facts.min = min;
    facts.max = max;
    facts.mean = n ? (float)(sum / n) : 0;
  }
  return facts;
}

bool ValueToImage(const Value& v, std::vector<uint8_t>& rgba, int& width, int& height) {
  const tensorflow::Tensor& t = v.tensor;
  if (t.dims() != 4 || t.dtype() != tensorflow::DT_FLOAT) return false;
  height = (int)t.dim_size(1);
  width = (int)t.dim_size(2);
  if ((int)t.dim_size(3) != 3 || width <= 0 || height <= 0) return false;
  const float* data = t.flat<float>().data();
  rgba.resize((size_t)width * height * 4);
  for (size_t i = 0; i < (size_t)width * height; ++i) {
    for (int c = 0; c < 3; ++c) {
      rgba[i * 4 + c] = (uint8_t)(std::clamp(data[i * 3 + c], 0.f, 1.f) * 255.f + 0.5f);
    }
    rgba[i * 4 + 3] = 255;
  }
  return true;
}

}  // namespace automat::tf
