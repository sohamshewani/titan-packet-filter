#ifndef TITAN_ENGINE_HPP
#define TITAN_ENGINE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <immintrin.h>

namespace titan {

enum class ErrorCode : uint32_t {
    Success = 0,
    ArenaExhausted,
    RingFull,
    RingEmpty,
    InvalidArguments,
    FlowTableFull,
    RateLimitExceeded,
    SystemError
};

template <typename T>
class Result {
private:
    union {
        T value_;
        ErrorCode error_;
    };
    bool has_value_;

public:
    constexpr Result(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : value_(val), has_value_(true) {}

    constexpr Result(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(val)), has_value_(true) {}

    constexpr Result(ErrorCode err) noexcept
        : error_(err), has_value_(false) {}

    ~Result() noexcept {
        if (has_value_) {
            value_.~T();
        }
    }

    constexpr Result(const Result& other) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(other.value_);
        } else {
            error_ = other.error_;
        }
    }

    constexpr Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(std::move(other.value_));
        } else {
            error_ = other.error_;
        }
        other.has_value_ = false;
    }

    Result& operator=(const Result& other) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        if (this != &other) {
            if (has_value_ && other.has_value_) {
                value_ = other.value_;
            } else if (has_value_) {
                value_.~T();
                error_ = other.error_;
            } else if (other.has_value_) {
                new (&value_) T(other.value_);
            } else {
                error_ = other.error_;
            }
            has_value_ = other.has_value_;
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            if (has_value_ && other.has_value_) {
                value_ = std::move(other.value_);
            } else if (has_value_) {
                value_.~T();
                error_ = other.error_;
            } else if (other.has_value_) {
                new (&value_) T(std::move(other.value_));
            } else {
                error_ = other.error_;
            }
            has_value_ = other.has_value_;
            other.has_value_ = false;
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
class Result<void> {
private:
    ErrorCode error_;
    bool has_value_;

public:
    constexpr Result() noexcept : error_(ErrorCode::Success), has_value_(true) {}
    constexpr Result(ErrorCode err) noexcept : error_(err), has_value_(false) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }
};

class Arena {
public:
    static constexpr size_t kSlotSize = 2048;
    static constexpr size_t kCacheLine = 64;

    struct alignas(kCacheLine) Slot {
        uint8_t data[kSlotSize];
    };

private:
    void* base_address_;
    size_t total_size_;
    size_t capacity_;
    Slot* slots_;
    std::atomic<uint64_t> free_bitmap_[64]; 
    std::atomic<uint64_t> exhausted_count_{0};

public:
    Arena(size_t num_slots) noexcept 
        : base_address_(nullptr), total_size_(0), capacity_(num_slots), slots_(nullptr) {
        if (num_slots == 0 || num_slots > 2048) {
            return;
        }

        size_t alloc_size = (num_slots * sizeof(Slot) + 4095) & ~size_t(4095);
        void* addr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        
        if (addr == MAP_FAILED) {
            addr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, 
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (addr == MAP_FAILED) {
                return;
            }
        }

        base_address_ = addr;
        total_size_ = alloc_size;
        slots_ = static_cast<Slot*>(base_address_);

        for (size_t i = 0; i < 64; ++i) {
            free_bitmap_[i].store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
        }
    }

    ~Arena() noexcept {
        if (base_address_ && base_address_ != MAP_FAILED) {
            munmap(base_address_, total_size_);
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    [[nodiscard]] Result<Slot*> allocate() noexcept {
        for (size_t word_idx = 0; word_idx < 32; ++word_idx) {
            uint64_t current = free_bitmap_[word_idx].load(std::memory_order_acquire);
            while (current != 0) {
                unsigned long bit_pos;
                if (_bit_scan_forward(&bit_pos, current)) {
                    uint64_t mask = ~(1ULL << bit_pos);
                    if (free_bitmap_[word_idx].compare_exchange_weak(
                            current, current & mask, 
                            std::memory_order_release, 
                            std::memory_order_acquire)) {
                        size_t slot_idx = (word_idx * 64) + bit_pos;
                        if (slot_idx < capacity_) {
                            return &slots_[slot_idx];
                        }
                    }
                } else {
                    break;
                }
                current = free_bitmap_[word_idx].load(std::memory_order_acquire);
            }
        }

        exhausted_count_.fetch_add(1, std::memory_order_relaxed);
        return ErrorCode::ArenaExhausted;
    }

    void deallocate(Slot* slot) noexcept {
        if (!slot || slot < slots_ || slot >= slots_ + capacity_) {
            return;
        }
        size_t slot_idx = static_cast<size_t>(slot - slots_);
        size_t word_idx = slot_idx / 64;
        size_t bit_pos = slot_idx % 64;

        uint64_t mask = 1ULL << bit_pos;
        uint64_t current = free_bitmap_[word_idx].load(std::memory_order_relaxed);
        while (!free_bitmap_[word_idx].compare_exchange_weak(
                current, current | mask, 
                std::memory_order_release, 
                std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] uint64_t exhausted_count() const noexcept {
        return exhausted_count_.load(std::memory_order_relaxed);
    }

private:
    static inline bool _bit_scan_forward(unsigned long* index, uint64_t mask) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        if (mask == 0) return false;
        *index = __builtin_ctzll(mask);
        return true;
#(_MSC_VER)
        return _BitScanForward64(index, mask) != 0;
#else
        if (mask == 0) return false;
        unsigned long i = 0;
        while ((mask & (1ULL << i)) == 0) {
            i++;
        }
        *index = i;
        return true;
#endif
    }
};

template <typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

private:
    struct Node {
        std::atomic<uint64_t> sequence;
        T data;
    };

    static constexpr size_t kCacheLine = 64;
    
    alignas(kCacheLine) Node buffer_[Capacity];
    alignas(kCacheLine) std::atomic<uint64_t> enqueue_pos_{0};
    alignas(kCacheLine) std::atomic<uint64_t> dequeue_pos_{0};
    alignas(kCacheLine) std::atomic<uint64_t> drop_count_{0};

public:
    RingBuffer() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~RingBuffer() noexcept = default;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    Result<void> enqueue(const T& item) noexcept {
        Node* node;
        uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            node = &buffer_[pos & (Capacity - 1)];
            uint64_t seq = node->sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, 
                                                       std::memory_order_relaxed, 
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                drop_count_.fetch_add(1, std::memory_order_relaxed);
                return ErrorCode::RingFull;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        node->data = item;
        node->sequence.store(pos + 1, std::memory_order_release);
        return ErrorCode::Success;
    }

    Result<void> enqueue(T&& item) noexcept {
        Node* node;
        uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            node = &buffer_[pos & (Capacity - 1)];
            uint64_t seq = node->sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, 
                                                       std::memory_order_relaxed, 
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                drop_count_.fetch_add(1, std::memory_order_relaxed);
                return ErrorCode::RingFull;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        node->data = std::move(item);
        node->sequence.store(pos + 1, std::memory_order_release);
        return ErrorCode::Success;
    }

    Result<T> dequeue() noexcept {
        Node* node;
        uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            node = &buffer_[pos & (Capacity - 1)];
            uint64_t seq = node->sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
            
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, 
                                                       std::memory_order_relaxed, 
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return ErrorCode::RingEmpty;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        T item = std::move(node->data);
        node->sequence.store(pos + Capacity, std::memory_order_release);
        return item;
    }

    [[nodiscard]] uint64_t drop_count() const noexcept {
        return drop_count_.load(std::memory_order_relaxed);
    }
};

class FlowController {
public:
    struct FlowKey {
        uint32_t src_ip;
        uint32_t dst_ip;
        uint16_t src_port;
        uint16_t dst_port;
        uint8_t protocol;

        [[nodiscard]] bool operator==(const FlowKey& o) const noexcept {
            return src_ip == o.src_ip && dst_ip == o.dst_ip &&
                   src_port == o.src_port && dst_port == o.dst_port &&
                   protocol == o.protocol;
        }
    };

    struct FlowState {
        std::atomic<uint64_t> last_seen_ns;
        std::atomic<uint64_t> packet_count;
        std::atomic<uint32_t> drop_count;
        uint32_t rate_limit_pps;
        uint8_t priority;
        bool active;
    };

    static constexpr size_t kBucketCount = 1024;
    static constexpr size_t kHopRange = 4;

    struct Bucket {
        std::atomic<uint32_t> lock;
        FlowKey key;
        FlowState state;
        uint32_t distance;
        bool occupied;
    };

private:
    Bucket buckets_[kBucketCount];
    std::atomic<uint64_t> global_drops_{0};

    [[nodiscard]] static constexpr uint32_t hash_