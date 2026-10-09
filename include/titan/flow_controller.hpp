#ifndef TITAN_FLOW_CONTROLLER_HPP
#define TITAN_FLOW_CONTROLLER_HPP

#include <cstdint>
#include <atomic>
#include <new>
#include <cstring>
#include <array>
#include <utility>

namespace titan {

enum class ErrorCode : uint32_t {
    Success = 0,
    RingFull,
    RingEmpty,
    ArenaExhausted,
    FlowNotFound,
    FlowTableFull,
    RateLimitExceeded,
    InvalidParameter
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

    constexpr Result(const Result& other) noexcept(false) : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(other.value_);
        } else {
            new (&error_) ErrorCode(other.error_);
        }
    }

    constexpr Result(Result&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(std::move(other.value_));
        } else {
            new (&error_) ErrorCode(other.error_);
        }
        other.has_value_ = false;
    }

    Result& operator=(const Result& other) noexcept(false) {
        if (this == &other) return *this;
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
        return *this;
    }

    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
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
        other.has_value_ = false;
        return *this;
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }
    [[nodiscard]] constexpr const T& value() const& noexcept { return value_; }
    [[nodiscard]] constexpr T& value() & noexcept { return value_; }
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return has_value_ ? ErrorCode::Success : error_; }
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
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return has_value_ ? ErrorCode::Success : error_; }
};

class alignas(64) Arena {
public:
    static constexpr size_t kSlotSize = 2048;
    static constexpr size_t kAlignment = 64;

    Arena(void* memory_pool, size_t total_bytes) noexcept 
        : pool_(static_cast<uint8_t*>(memory_pool)), 
          total_slots_(total_bytes / kSlotSize),
          free_head_(0) {
        for (size_t i = 0; i < total_slots_; ++i) {
            uint32_t* next_ptr = reinterpret_cast<uint32_t*>(pool_ + (i * kSlotSize));
            *next_ptr = static_cast<uint32_t>(i + 1);
        }
    }

    ~Arena() noexcept = default;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    [[nodiscard]] void* allocate() noexcept {
        uint32_t current_head = free_head_.load(std::memory_order_relaxed);
        while (current_head < total_slots_) {
            uint8_t* slot_addr = pool_ + (current_head * kSlotSize);
            uint32_t next_free = *reinterpret_cast<uint32_t*>(slot_addr);
            
            if (free_head_.compare_exchange_weak(current_head, next_free,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                std::memset(slot_addr, 0, kSlotSize);
                return static_cast<void*>(slot_addr);
            }
        }
        return nullptr;
    }

    void deallocate(void* ptr) noexcept {
        if (!ptr) return;
        uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);
        size_t offset = byte_ptr - pool_;
        uint32_t slot_index = static_cast<uint32_t>(offset / kSlotSize);

        uint32_t current_head = free_head_.load(std::memory_order_relaxed);
        do {
            *reinterpret_cast<uint32_t*>(byte_ptr) = current_head;
        } while (!free_head_.compare_exchange_weak(current_head, slot_index,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
    }

    [[nodiscard]] size_t capacity() const noexcept { return total_slots_; }

private:
    uint8_t* pool_;
    size_t total_slots_;
    alignas(64) std::atomic<uint32_t> free_head_;
};

template <size_t Capacity>
class alignas(64) RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "RingBuffer capacity must be a power of two.");

public:
    struct Entry {
        std::atomic<uint64_t> sequence;
        void* data;
    };

    RingBuffer() noexcept : head_(0), tail_(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
            buffer_[i].data = nullptr;
        }
    }

    ~RingBuffer() noexcept = default;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    [[nodiscard]] Result<void> push(void* ptr) noexcept {
        Entry* entry;
        uint64_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            entry = &buffer_[pos & (Capacity - 1)];
            uint64_t seq = entry->sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return ErrorCode::RingFull;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        entry->data = ptr;
        entry->sequence.store(pos + 1, std::memory_order_release);
        return Result<void>();
    }

    [[nodiscard]] Result<void*> pop() noexcept {
        Entry* entry;
        uint64_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            entry = &buffer_[pos & (Capacity - 1)];
            uint64_t seq = entry->sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
            
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return ErrorCode::RingEmpty;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        void* ptr = entry->data;
        entry->data = nullptr;
        entry->sequence.store(pos + Capacity, std::memory_order_release);
        return Result<void*>(ptr);
    }

    [[nodiscard]] size_t size() const noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail >= head) {
            return static_cast<size_t>(tail - head);
        }
        return 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
    std::array<Entry, Capacity> buffer_;
};

class alignas(64) FlowController {
public:
    struct FlowState {
        uint32_t flow_id;
        std::atomic<uint64_t> packet_count;
        std::atomic<uint64_t> drop_count;
        std::atomic<uint64_t> last_timestamp_ns;
        std::atomic<uint32_t> tokens;
        uint32_t max_tokens;
        uint32_t refill_rate;
    };

    static constexpr size_t kBucketSize = 4;
    static constexpr size_t kTableCapacityBits = 12;
    static constexpr size_t kTableSize = 1 << kTableCapacityBits;
    static constexpr size_t kHopRange = 32;

    struct Bucket {
        std::atomic<uint32_t> metadata; 
        std::array<FlowState, kBucketSize> flows;
    };

    FlowController() noexcept : total_drops_(0), total_allowed_(0) {
        for (size_t i = 0; i < kTableSize; ++i) {
            buckets_[i].metadata.store(0, std::memory_order_relaxed);
            for (size_t j = 0; j < kBucketSize; ++j) {
                buckets_[i].flows[j].flow_id = 0;
                buckets_[i].flows[j].packet_count.store(0, std::memory_order_relaxed);
                buckets_[i].flows[j].drop_count.store(0, std::memory_order_relaxed);
                buckets_[i].flows[j].last_timestamp_ns.store(0, std::memory_order_relaxed);
                buckets_[i].flows[j].tokens.store(0, std::memory_order_relaxed);
                buckets_[i].flows[j].max_tokens = 100;
                buckets_[i].flows[j].refill_rate = 10;
            }
        }
    }

    ~FlowController() noexcept = default;

    FlowController(const FlowController&) = delete;
    FlowController& operator=(const FlowController&) = delete;
    FlowController(FlowController&&) = delete;
    FlowController& operator=(FlowController&&) = delete;

    [[nodiscard]] Result<void> register_flow(uint32_t flow_id, uint32_t max_tokens, uint32_t refill_rate) noexcept {
        uint32_t hash = hash_flow(flow_id);
        size_t start_bucket = hash & (kTableSize - 1);

        for (size_t step = 0; step < kHopRange; ++step) {
            size_t bucket_idx = (start_bucket + step) & (kTableSize - 1);
            Bucket& bucket = buckets_[bucket_idx];

            for (size_t i = 0; i < kBucketSize; ++i) {
                uint32_t current_id = bucket.flows[i].flow_id;
                if (current_id == 0 || current_id == flow_id) {
                    bucket.flows[i].flow_id = flow_id;
                    bucket.flows[i].max_tokens = max_tokens;
                    bucket.flows[i].refill_rate = refill_rate;
                    bucket.flows[i].tokens.store(max_tokens, std::memory_order_relaxed);
                    return Result<void>();
                }
            }
        }
        return ErrorCode::FlowTableFull;
    }

    [[nodiscard]] bool evaluate_packet(uint32_t flow_id, uint64_t current_time_ns) noexcept {
        FlowState* flow = lookup_flow(flow_id);
        if (!flow) {
            total_drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        uint64_t last_ts = flow->last_timestamp_ns.load(std::memory_order_relaxed);
        if (current_time_ns > last_ts) {
            uint64_t delta_ns = current_time_ns - last_ts;
            uint32_t generated_tokens = static_cast<uint32_t>((delta_ns * flow->refill_rate) / 1000000000ULL);
            if (generated_tokens > 0) {
                uint32_t current_tokens = flow->tokens.load(std::memory_order_relaxed);
                uint32_t new_tokens = std::min(flow->max_tokens, current_tokens + generated_tokens);
                flow->tokens.store(new_tokens, std::memory_order_relaxed);
                flow->last_timestamp_ns.store(current_time_ns, std::memory_order_relaxed);
            }
        }

        uint32_t tokens = flow->tokens.load(std::memory_order_relaxed);
        if (tokens > 0) {
            if (flow->tokens.compare_exchange_weak(tokens, tokens - 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                flow->packet_count.fetch_add(1, std::memory_order_relaxed);
                total_allowed_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }

        flow->drop_count.fetch_add(1, std::memory_order_relaxed);
        total_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] uint64_t total_drops() const noexcept {
        return total_drops_.load(std::memory_order_relaxed);
    }

    [[