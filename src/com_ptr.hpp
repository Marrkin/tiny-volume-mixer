#pragma once

#include <utility>

namespace tvm {
// Small, owning COM pointer. All audio interfaces stay in the worker's MTA.
template <class T> class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* value) : value_(value) { if (value_) value_->AddRef(); }
    ComPtr(const ComPtr& other) : ComPtr(other.value_) {}
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ~ComPtr() { reset(); }
    ComPtr& operator=(ComPtr other) noexcept { std::swap(value_, other.value_); return *this; }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** put() { reset(); return &value_; }
    void attach(T* value) { reset(); value_ = value; }
    T* detach() { return std::exchange(value_, nullptr); }
    void reset() { if (auto* old = std::exchange(value_, nullptr)) old->Release(); }
private:
    T* value_ = nullptr;
};
} // namespace tvm
