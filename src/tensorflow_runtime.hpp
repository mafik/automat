#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// A thin quarantine around the TensorFlow C++ API. TensorFlow's headers define
// their own logging macros (LOG/CHECK) and use enum arithmetic that Automat's
// macros and -std=gnu++26 reject, so every translation unit that includes them
// is confined to tensorflow_runtime.cpp - built on its own as gnu++17 (see
// src/tensorflow.py) - and the rest of Automat sees only the plain types below.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace automat::tf {

// Opaque handle to a tensorflow::Tensor held on the CPU.
struct Value;

// The printed facts of one value; see docs/parrots/Pipeline Language.md.
struct Facts {
  std::string format;  // "f32[1,240,320,3]"
  std::string device;  // "CPU"
  float min = 0, mean = 0, max = 0;
};

// Builds an NHWC [1,height,width,3] float32 tensor from RGBA8888 pixels (row
// stride width*4).
std::shared_ptr<Value> ImageToValue(const uint8_t* rgba, int width, int height);

// Runs the op named `op_type` (looked up in TensorFlow's op registry) on
// `input` once on the CPU. Returns the output value, or null with `error` set
// when the op is unknown or the run fails.
std::shared_ptr<Value> RunUnaryOp(const std::string& op_type, const Value& input,
                                  std::string& error);

// The format, device and value range of `v`.
Facts Describe(const Value& v);

// If `v` is a [1,h,w,3] float value, writes clamped RGBA8888 pixels into `rgba`
// and returns true; otherwise returns false.
bool ValueToImage(const Value& v, std::vector<uint8_t>& rgba, int& width, int& height);

}  // namespace automat::tf
