// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Vector with inline storage for the first N elements.
//
// Most manta constructs are small: a device has one or two terminals, a chain a
// handful of elements, a bundle one wire. Keeping those on the stack avoids a
// heap round trip in the hottest loops of chain evaluation.
#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace manta {

template <typename T, std::size_t N = 4>
class SmallVec {
    static_assert(std::is_nothrow_move_constructible_v<T>);

public:
    SmallVec() = default;

    SmallVec(std::initializer_list<T> init) {
        reserve(init.size());
        for (const auto& v : init) push_back(v);
    }

    SmallVec(const SmallVec& o) {
        reserve(o.size_);
        for (std::size_t i = 0; i < o.size_; ++i) std::construct_at(data_ + i, o.data_[i]);
        size_ = o.size_;
    }

    SmallVec(SmallVec&& o) noexcept {
        if (o.heap()) {
            data_ = o.data_;
            size_ = o.size_;
            cap_ = o.cap_;
            o.data_ = o.inlineData();
            o.size_ = 0;
            o.cap_ = N;
        } else {
            for (std::size_t i = 0; i < o.size_; ++i)
                std::construct_at(data_ + i, std::move(o.data_[i]));
            size_ = o.size_;
            o.clear();
        }
    }

    SmallVec& operator=(SmallVec o) noexcept {
        swap(o);
        return *this;
    }

    ~SmallVec() {
        clear();
        if (heap()) ::operator delete(data_, std::align_val_t{alignof(T)});
    }

    void swap(SmallVec& o) noexcept {
        SmallVec tmp(std::move(o));
        o.assignFrom(std::move(*this));
        assignFrom(std::move(tmp));
    }

    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == cap_) grow(cap_ * 2);
        std::construct_at(data_ + size_, std::forward<Args>(args)...);
        return data_[size_++];
    }

    void pop_back() {
        --size_;
        std::destroy_at(data_ + size_);
    }

    void reserve(std::size_t n) {
        if (n > cap_) grow(n);
    }

    void resize(std::size_t n, const T& fill = T{}) {
        reserve(n);
        while (size_ < n) std::construct_at(data_ + size_++, fill);
        while (size_ > n) pop_back();
    }

    void clear() {
        std::destroy_n(data_, size_);
        size_ = 0;
    }

    void append(std::span<const T> src) {
        reserve(size_ + src.size());
        for (const auto& v : src) std::construct_at(data_ + size_++, v);
    }

    [[nodiscard]] T& operator[](std::size_t i) { return data_[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const { return data_[i]; }
    [[nodiscard]] T& front() { return data_[0]; }
    [[nodiscard]] const T& front() const { return data_[0]; }
    [[nodiscard]] T& back() { return data_[size_ - 1]; }
    [[nodiscard]] const T& back() const { return data_[size_ - 1]; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }

    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return data_ + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }

    operator std::span<const T>() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<T> span() noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const T> span() const noexcept { return {data_, size_}; }

private:
    [[nodiscard]] T* inlineData() noexcept { return reinterpret_cast<T*>(storage_); }
    [[nodiscard]] bool heap() const noexcept {
        return data_ != reinterpret_cast<const T*>(storage_);
    }

    void assignFrom(SmallVec&& o) noexcept {
        clear();
        if (heap()) ::operator delete(data_, std::align_val_t{alignof(T)});
        if (o.heap()) {
            data_ = o.data_;
            cap_ = o.cap_;
            size_ = o.size_;
            o.data_ = o.inlineData();
            o.cap_ = N;
            o.size_ = 0;
        } else {
            data_ = inlineData();
            cap_ = N;
            size_ = o.size_;
            for (std::size_t i = 0; i < size_; ++i)
                std::construct_at(data_ + i, std::move(o.data_[i]));
            o.clear();
        }
    }

    void grow(std::size_t want) {
        std::size_t n = std::max(want, cap_ + cap_ / 2 + 1);
        T* fresh = static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(fresh + i, std::move(data_[i]));
            std::destroy_at(data_ + i);
        }
        if (heap()) ::operator delete(data_, std::align_val_t{alignof(T)});
        data_ = fresh;
        cap_ = n;
    }

    alignas(T) std::byte storage_[N * sizeof(T)]{};
    T* data_ = reinterpret_cast<T*>(storage_);
    std::size_t size_ = 0;
    std::size_t cap_ = N;
};

}  // namespace manta
