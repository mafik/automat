#pragma once

#include <compare>
#include <cstring>
#include <span>
#include <string_view>

#include "format.hh"
#include "int.hh"
#include "status.hh"
#include "str.hh"

namespace maf {

constexpr Size DynamicExtent = std::dynamic_extent;

// Wrapper around std::span with some quality-of-life improvements.
template <class T = char, Size Extent = DynamicExtent>
struct Span : std::span<T, Extent> {
  using std::span<T, Extent>::span;

  inline Span(const Str& s) : Span(const_cast<char*>(s.data()), s.size() / sizeof(T)) {}
  inline Span(StrView s) : Span(const_cast<char*>(s.data()), s.size() / sizeof(T)) {}

  // Arrays
  template <Size N>
  inline Span(T (&arr)[N]) : std::span<T, DynamicExtent>(arr, N) {}

  template <Size ExtentRhs>
  inline Span(std::span<T, ExtentRhs> s) : std::span<T, Extent>(s) {}

  template <Size ExtentRhs>
  inline Span& operator=(const std::span<T, ExtentRhs>& rhs) {
    std::span<T, Extent>::operator=(rhs);
    return *this;
  }

  auto RemovePrefix(Size n) {
    *this = this->subspan(n);
    return *this;
  }

  template <Size ExtentRhs>
  constexpr inline bool StartsWith(Span<T, ExtentRhs> prefix) {
    if (this->size() < prefix.size()) {
      return false;
    }
    return std::equal(prefix.begin(), prefix.end(), this->begin());
  }

  template <typename U>
  U& As(Status& status) {
    if (this->size() < sizeof(U)) {
      AppendErrorMessage(status) +=
          f("Span too small to contain %s (%x vs %x)", typeid(U).name(), this->size(), sizeof(U));
      // TODO: return nullptr
    }
    return *(U*)this->data();
  }

  // Unchecked version of As. Use only when you know the span is big enough.
  template <typename U>
  U& As() {
    return *(U*)this->data();
  }

  template <typename U>
  U& Consume(Status& status) {
    U& ret = *(U*)this->data();
    if (this->size() < sizeof(U)) {
      AppendErrorMessage(status) +=
          f("Span too small to contain %s (%x vs %x)", typeid(U).name(), this->size(), sizeof(U));
      this->RemovePrefix(this->size());
      // TODO: return nullptr
    } else {
      this->RemovePrefix(sizeof(U));
    }
    return ret;
  }

  // Unchecked version of Consume. Use only when you know the span is big
  // enough.
  template <typename U>
  U& Consume() {
    U* ret = (U*)this->data();
    RemovePrefix(sizeof(U));
    return *ret;
  }

  template <typename U>
  void PutRef(const U& ref) {
    memcpy(this->data(), &ref, sizeof(U));
  }

  bool Empty() const { return this->size() == 0; }
  bool Filled() const { return !this->empty(); }

  Span<> ConsumeSpan(Size n, Status& status) {
    Span<> ret = this->first(n);
    if (this->size() < n) {
      AppendErrorMessage(status) += f("Span too small (%x vs %x)", this->size(), n);
      this->RemovePrefix(this->size());
    } else {
      this->RemovePrefix(n);
    }
    return ret;
  }

  template <class T2 = char, Size Extent2 = DynamicExtent>
  std::strong_ordering operator<=>(Span<T2, Extent2> other) const {
    return std::lexicographical_compare_three_way(this->begin(), this->end(), other.begin(),
                                                  other.end());
  }

  template <class T2 = char, Size Extent2 = DynamicExtent>
  bool operator==(Span<T2, Extent2> other) const {
    return std::equal(this->begin(), this->end(), other.begin(), other.end());
  }

  Str ToStr() const { return Str(this->data(), this->size() * sizeof(T)); }
};

constexpr Span<char, 0> kEmptySpan;

// Span of a C string, excluding the null terminator.
template <size_t N>
constexpr inline Span<char, N - 1> SpanOfCStr(const char (&c_str)[N]) {
  return Span<char, N - 1>(const_cast<char*>(c_str), N - 1);
}

// Span of an arbitrary type passed by reference.
template <typename T>
constexpr inline Span<char, sizeof(T)> SpanOfRef(const T& x) {
  return Span<char, sizeof(T)>((char*)&x, sizeof(T));
}

inline StrView StrViewOf(Span<> span) { return StrView(span.data(), span.size()); }

template <typename T>
inline Span<T, DynamicExtent> SpanOfArr(T* arr, Size n) {
  return Span<T, DynamicExtent>(arr, n);
}

}  // namespace maf

template <>
struct std::hash<maf::Span<>> {
  std::size_t operator()(maf::Span<> span) const {
    return std::hash<std::string_view>()(StrViewOf(span));
  }
};