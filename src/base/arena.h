// Bump allocator for AST and IR nodes.
//
// The compiler allocates a great many small, permanent nodes and frees them all
// at once when the translation unit is done. A bump allocator makes allocation
// a pointer increment and deallocation free, and keeps nodes contiguous so that
// tree walks stay cache-resident.
//
// Objects allocated here are never destroyed: only trivially-destructible types
// are permitted, which is enforced at compile time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace manta {

class Arena {
public:
    static constexpr std::size_t kDefaultBlock = 64 * 1024;

    Arena() = default;
    explicit Arena(std::size_t firstBlock) : nextBlockSize_(firstBlock) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& o) noexcept
        : blocks_(std::move(o.blocks_)), cur_(o.cur_), end_(o.end_),
          nextBlockSize_(o.nextBlockSize_), used_(o.used_) {
        o.cur_ = o.end_ = nullptr;
    }

    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) {
            release();
            blocks_ = std::move(o.blocks_);
            cur_ = o.cur_;
            end_ = o.end_;
            nextBlockSize_ = o.nextBlockSize_;
            used_ = o.used_;
            o.cur_ = o.end_ = nullptr;
        }
        return *this;
    }

    ~Arena() { release(); }

    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align) {
        std::uintptr_t p = reinterpret_cast<std::uintptr_t>(cur_);
        std::uintptr_t aligned = (p + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        if (aligned + bytes > reinterpret_cast<std::uintptr_t>(end_)) [[unlikely]] {
            grow(bytes, align);
            p = reinterpret_cast<std::uintptr_t>(cur_);
            aligned = (p + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        }
        cur_ = reinterpret_cast<std::byte*>(aligned + bytes);
        used_ += bytes;
        return reinterpret_cast<void*>(aligned);
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena never runs destructors; use a trivially-destructible type");
        void* mem = allocate(sizeof(T), alignof(T));
        return std::construct_at(static_cast<T*>(mem), std::forward<Args>(args)...);
    }

    // Uninitialised array. Callers fill it; the elements are never destroyed.
    template <typename T>
    [[nodiscard]] std::span<T> makeArray(std::size_t n) {
        static_assert(std::is_trivially_destructible_v<T>);
        if (n == 0) return {};
        void* mem = allocate(sizeof(T) * n, alignof(T));
        return std::span<T>(static_cast<T*>(mem), n);
    }

    // Copy of a range into arena storage.
    template <typename T>
    [[nodiscard]] std::span<T> copyArray(std::span<const T> src) {
        auto dst = makeArray<T>(src.size());
        for (std::size_t i = 0; i < src.size(); ++i) dst[i] = src[i];
        return dst;
    }

    [[nodiscard]] std::size_t bytesUsed() const noexcept { return used_; }
    [[nodiscard]] std::size_t blockCount() const noexcept { return blocks_.size(); }

    void reset() {
        release();
        nextBlockSize_ = kDefaultBlock;
        used_ = 0;
    }

private:
    void grow(std::size_t bytes, std::size_t align) {
        std::size_t need = bytes + align;
        std::size_t size = nextBlockSize_ > need ? nextBlockSize_ : need;
        auto* block = static_cast<std::byte*>(::operator new(size, std::align_val_t{alignof(std::max_align_t)}));
        blocks_.push_back(block);
        blockSizes_.push_back(size);
        cur_ = block;
        end_ = block + size;
        if (nextBlockSize_ < 4u * 1024u * 1024u) nextBlockSize_ *= 2;
    }

    void release() {
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            ::operator delete(blocks_[i], std::align_val_t{alignof(std::max_align_t)});
        }
        blocks_.clear();
        blockSizes_.clear();
        cur_ = end_ = nullptr;
    }

    std::vector<std::byte*> blocks_;
    std::vector<std::size_t> blockSizes_;
    std::byte* cur_ = nullptr;
    std::byte* end_ = nullptr;
    std::size_t nextBlockSize_ = kDefaultBlock;
    std::size_t used_ = 0;
};

}  // namespace manta
