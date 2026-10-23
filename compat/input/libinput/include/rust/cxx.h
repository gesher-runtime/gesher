// Gesher: what libinput's headers use of the cxx crate's C++ side
// (rust/cxx.h). libinput's Rust half is not built (no Rust toolchain);
// InputVerifier.h holds its verifier in a rust::Box, which here owns a C++
// object (compat/input/libinput/InputVerifier.cc). docs/input.md.
#pragma once

#include <memory>
#include <utility>

namespace rust {
inline namespace cxxbridge1 {

// An owning pointer. The object is deleted by the deleter chosen where it was
// made, so a Box of a type that is incomplete where the Box is destroyed is
// fine.
template <typename T>
class Box {
 public:
  explicit Box(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_.get(); }

 private:
  std::shared_ptr<T> ptr_;
};

}  // namespace cxxbridge1
}  // namespace rust
