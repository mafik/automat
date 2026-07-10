// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "tensorflow_runtime.hpp"

#include <tensorflow/core/framework/graph.pb.h>
#include <tensorflow/core/framework/op.h>
#include <tensorflow/core/framework/tensor.h>
#include <tensorflow/core/graph/graph.h>
#include <tensorflow/core/graph/node_builder.h>
#include <tensorflow/core/public/session.h>

#include <algorithm>

#include "tensorflow_embed.h"
#include "tensorflow_trampolines.hpp"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <delayimp.h>  // uses the windows.h types, so it must follow it
// clang-format on

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#endif

namespace automat::tf {

#ifdef _WIN32

// automat.exe delay-loads tensorflow.dll (src/tensorflow.py wires /DELAYLOAD);
// the DLL ships embedded in the binary (tensorflow_embed.c) and the hook below
// serves the load from a cached extraction, because Windows cannot load a PE
// image straight from memory. The delay-load thunks are the platform's own
// trampolines; on Linux the same routing is done by the hand trampolines in
// tensorflow_trampolines.cpp.

static std::filesystem::path CachePath() {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < tf_library_size; ++i) {
    hash = (hash ^ tf_library[i]) * 1099511628211ull;
  }
  std::filesystem::path dir;
  if (const wchar_t* local_app_data = _wgetenv(L"LOCALAPPDATA")) {
    dir = std::filesystem::path(local_app_data) / "Automat";
  } else {
    dir = std::filesystem::temp_directory_path() / "Automat";
  }
  char name[64];
  snprintf(name, sizeof(name), "tensorflow-%016llx.dll", (unsigned long long)hash);
  return dir / name;
}

static HMODULE LoadEmbeddedLibrary() {
  static HMODULE module = []() -> HMODULE {
    auto path = CachePath();
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != tf_library_size) {
      std::filesystem::create_directories(path.parent_path(), ec);
      auto tmp = path;
      tmp += ".tmp" + std::to_string(GetCurrentProcessId());
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write((const char*)tf_library, tf_library_size);
        if (!out) {
          fprintf(stderr, "Extracting TensorFlow to %s failed\n", tmp.string().c_str());
          return nullptr;
        }
      }
      std::filesystem::rename(tmp, path, ec);
      if (ec) {
        // Another process may have won the race; the tmp copy is then redundant.
        std::filesystem::remove(tmp, ec);
        if (std::filesystem::file_size(path, ec) != tf_library_size) {
          fprintf(stderr, "Placing TensorFlow at %s failed\n", path.string().c_str());
          return nullptr;
        }
      }
    }
    HMODULE m = LoadLibraryW(path.c_str());
    if (m == nullptr) {
      fprintf(stderr, "LoadLibrary(%s) failed, error %lu\n", path.string().c_str(), GetLastError());
    }
    return m;
  }();
  return module;
}

static FARPROC WINAPI DelayLoadHook(unsigned notify, DelayLoadInfo* info) {
  if (notify == dliNotePreLoadLibrary && _stricmp(info->szDll, "tensorflow.dll") == 0) {
    return (FARPROC)LoadEmbeddedLibrary();
  }
  return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = DelayLoadHook;

// Guards every entry point so a failed extraction reads as an error instead of
// a delay-load exception on the first TensorFlow call.
static bool Available() { return LoadEmbeddedLibrary() != nullptr; }

#else

// Automat's TensorFlow calls route through the hand trampolines, which jump to
// the addresses bound here. The library ships embedded (tensorflow_embed.c) and
// loads from memory (memfd) on the first call; it is self-contained, so a
// private (RTLD_LOCAL) load keeps its symbols out of the global scope, and the
// binary keeps its normal, fully-resolved (-z now) link.
static bool Available() {
  static bool ok = [] {
    int fd = memfd_create("libtensorflow.so", MFD_CLOEXEC);
    if (fd < 0) {
      fprintf(stderr, "memfd_create failed: %s\n", strerror(errno));
      return false;
    }
    size_t written = 0;
    while (written < tf_library_size) {
      ssize_t n = write(fd, tf_library + written, tf_library_size - written);
      if (n <= 0) {
        fprintf(stderr, "Writing TensorFlow to a memfd failed: %s\n", strerror(errno));
        close(fd);
        return false;
      }
      written += n;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    close(fd);
    if (handle == nullptr) {
      fprintf(stderr, "dlopen of the embedded TensorFlow failed: %s\n", dlerror());
      return false;
    }
    return tf_bind_symbols(handle);
  }();
  return ok;
}

#endif

struct Value {
  tensorflow::Tensor tensor;
};

std::shared_ptr<Value> ImageToValue(const uint8_t* rgba, int width, int height) {
  if (!Available()) return nullptr;
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
  if (!Available()) {
    error = "TensorFlow could not be loaded";
    return nullptr;
  }
  using namespace tensorflow;
  Graph graph(OpRegistry::Global());
  Node* placeholder = nullptr;
  Status s = NodeBuilder("input", "Placeholder")
                 .Attr("dtype", input.tensor.dtype())
                 .Finalize(&graph, &placeholder);
  Node* node = nullptr;
  if (s.ok()) {
    s = NodeBuilder("op", op_type).Input(placeholder).Finalize(&graph, &node);
  }
  if (!s.ok()) {
    error = op_type + ": " + std::string(s.message());
    return nullptr;
  }
  GraphDef graph_def;
  graph.ToGraphDef(&graph_def);
  std::unique_ptr<Session> session(NewSession(SessionOptions()));
  s = session->Create(graph_def);
  if (!s.ok()) {
    error = op_type + ": " + std::string(s.message());
    return nullptr;
  }
  std::vector<Tensor> outputs;
  s = session->Run({{placeholder->name(), input.tensor}}, {node->name() + ":0"}, {}, &outputs);
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
  if (!Available()) return {};
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
  if (!Available()) return false;
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
