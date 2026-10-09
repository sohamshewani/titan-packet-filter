#ifndef TITAN_RING_BUFFER_HPP
#define TITAN_RING_BUFFER_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>
#include <type_traits>

namespace titan {

enum class ErrorCode : uint32_t {
    Success = 0,
    RingFull,
    RingEmpty,
    InvalidArgument,
    ArenaExhausted,
    Timeout
};

template <typename T, ErrorCode E>
class Result {
private:
    union {
        T value_;
        ErrorCode error_;
    };
    bool has_value_;

public:
    constexpr Result(const T& val) noexcept : value_(val), has_value_(true) {}
    constexpr Result(T&& val) noexcept : value_(std::move(val)), has_value_(true) {}
    constexpr Result(ErrorCode err) noexcept : error_(err), has_value_(false) {}

    ~Result() noexcept {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~ErrorCode();
        }
    }

    Result(const Result& other) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(other.value_);
        } else {
            new (&error_) ErrorCode(other.error_);
        }
    }

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(std::move(other.value_));
        } else {
            new (&error_) ErrorCode(other.error_);
        }
    }

    Result& operator=(const Result& other) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        if (this != &other) {
            if (has_value_ && other.has_value_) {
                value_ = other.value_;
            } else if (!has_value_ && !other.has_value_) {
                error_ = other.error_;
            } else {
                if (has_value_) {
                    value_.~T();
                } else {
                    error_.~ErrorCode();
                }
                has_value_ = other.has_value_;
                if (has_value_) {
                    new (&value_) T(other.value_);
                } else {
                    new (&error_) ErrorCode(other.error_);
                }
            }
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            if (has_value_ && other.has_value_) {
                value_ = std::move(other.value_);
            } else if (!has_value_ && !other.has_value_) {
                error_ = other.error_;
            } else {
                if (has_value_) {
                    value_.~T();
                } else {
                    error_.~ErrorCode();
                }
                has_value_ = other.has_value_;
                if (has_value_) {
                    new (&value_) T(std::move(other.value_));
                } else {
                    new (&error_) ErrorCode(other.error_);
                }
            }
        }
        return *this;
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }

    [[nodiscard]] constexpr T& value() & noexcept {
        return value_;
    }

    [[nodiscard]] constexpr const T& value() const & noexcept {
        return value_;
    }

    [[nodiscard]] constexpr T&& value() && noexcept {
        return std::move(value_);
    }

    [[nodiscard]] constexpr ErrorCode error() const noexcept {
        return error_;
    }
};

template <>
class Result<void, ErrorCode> {
private:
    ErrorCode error_;
    bool has_value_;

public:
    constexpr Result() noexcept : error_(ErrorCode::Success), has_value_(true) {}
    constexpr Result(ErrorCode err) noexcept : error_(err), has_value_(err == ErrorCode::Success) {}

    ~Result() noexcept = default;
    Result(const Result&) noexcept = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }
};

template <typename T, size_t Capacity>
class RingBuffer {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "RingBuffer capacity must be a power of two.");

    struct Cell {
        std::atomic<uint64_t> sequence;
        T data;
    };

    alignas(64) Cell* buffer_;
    const size_t capacity_;
    const size_t mask_;

    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;

    alignas(64) std::atomic<uint64_t> dropped_packets_;

public:
    RingBuffer() noexcept 
        : capacity_(Capacity), 
          mask_(Capacity - 1), 
          head_(0), 
          tail_(0), 
          dropped_packets_(0) {
        
        size_t alloc_size = sizeof(Cell) * capacity_;
        void* ptr = ::operator new(alloc_size, std::align_val_t{64}, std::nothrow);
        if (ptr == nullptr) {
            std::abort();
        }
        buffer_ = static_cast<Cell*>(ptr);

        for (size_t i = 0; i < capacity_; ++i) {
            new (&buffer_[i].sequence) std::atomic<uint64_t>(i);
        }
    }

    ~RingBuffer() noexcept {
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.~atomic();
            if constexpr (!std::is_trivially_destructible_v<T>) {
                buffer_[i].data.~T();
            }
        }
        ::operator delete(buffer_, std::align_val_t{64});
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    [[nodiscard]] Result<void, ErrorCode> push(const T& item) noexcept {
        Cell* cell;
        uint64_t pos = head_.load(std::memory_order_relaxed);
        
        for (;;) {
            cell = &buffer_[pos & mask_];
            uint64_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, 
                                                std::memory_order_relaxed, 
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                return ErrorCode::RingFull;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return ErrorCode::Success;
    }

    [[nodiscard]] Result<void, ErrorCode> push(T&& item) noexcept {
        Cell* cell;
        uint64_t pos = head_.load(std::memory_order_relaxed);
        
        for (;;) {
            cell = &buffer_[pos & mask_];
            uint64_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, 
                                                std::memory_order_relaxed, 
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                return ErrorCode::RingFull;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return ErrorCode::Success;
    }

    [[nodiscard]] Result<T, ErrorCode> pop() noexcept {
        Cell* cell;
        uint64_t pos = tail_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & mask_];
            uint64_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, 
                                                std::memory_order_relaxed, 
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return ErrorCode::RingEmpty;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        T item = std::move(cell->data);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] size_t size() const noexcept {
        uint64_t head = head_.load(std::memory_order_acquire);
        uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head >= tail) {
            return static_cast<size_t>(head - tail);
        }
        return 0;
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t dropped_packets() const noexcept {
        return dropped_packets_.load(std::memory_order_relaxed);
    }

    void reset_metrics() noexcept {
        dropped_packets_.store(0, std::memory_order_relaxed);
    }
};

} // namespace titan

#endif // TITAN_RING_BUFFER_HPP