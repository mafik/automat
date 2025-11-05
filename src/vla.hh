#pragma once
// Header with structures that keep their data on stack

namespace automat {

template <typename T>
struct VlaStack {
  int size = 0;
  T* data;

  void Push(T value) { data[size++] = value; }
  int Size() { return size; }

  operator bool() { return size != 0; }

  using it = T*;

  it begin() { return data; }
  it end() { return data + size; }
};

// Helper for using tiny, stack-based (sic!) stacks.
#define VLA_STACK(name, T, N) \
  T name##_data[N];           \
  VlaStack<T> name{0, name##_data};

}  // namespace automat
