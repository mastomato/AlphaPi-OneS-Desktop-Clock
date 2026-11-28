#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace radio {

// Fixed-capacity FIFO. The player holds its queue mutex around every access.
// A full queue applies backpressure instead of discarding audio. Reading only
// advances an index; it never moves the remaining samples in PSRAM.
template <typename T>
class StreamQueue {
public:
    constexpr void Init(size_t capacity) { storage_.resize(capacity); }
    constexpr void Clear() { head_ = size_ = 0; }
    constexpr size_t size() const { return size_; }

    constexpr size_t Push(const T* data, size_t count) {
        count = std::min(count, storage_.size() - size_);
        if (count == 0) {
            return 0;
        }
        const size_t tail = (head_ + size_) % storage_.size();
        const size_t first = std::min(count, storage_.size() - tail);
        std::copy_n(data, first, storage_.data() + tail);
        std::copy_n(data + first, count - first, storage_.data());
        size_ += count;
        return count;
    }

    constexpr size_t Pop(T* data, size_t count) {
        count = std::min(count, size_);
        if (count == 0) {
            return 0;
        }
        const size_t first = std::min(count, storage_.size() - head_);
        std::copy_n(storage_.data() + head_, first, data);
        std::copy_n(storage_.data(), count - first, data + first);
        head_ = (head_ + count) % storage_.size();
        size_ -= count;
        return count;
    }

private:
    std::vector<T> storage_;
    size_t head_ = 0;
    size_t size_ = 0;
};

}  // namespace radio
