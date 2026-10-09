#ifndef TITAN_ARENA_HPP
#define TITAN_ARENA_HPP

#include <cstdint>
#include <cstddef>
#include <new>
#include <system_error>
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>

namespace titan {

enum class ErrorCode : uint32_t {
    Success = 0,
    OutOfMemory,
    InvalidAlignment,
    InvalidSize,
    ArenaExhausted,
    RingBufferFull,
    RingBufferEmpty,
    FlowNotFound,
    FlowTableFull,
    InvalidArgument
};

class ErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "titan";
    }

    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::Success: return "Success";
            case ErrorCode::OutOfMemory: return "Out of memory (mmap failed)";
            case ErrorCode::InvalidAlignment: return "Invalid alignment constraint";
            case ErrorCode::InvalidSize: return "Invalid size parameter";
            case ErrorCode::ArenaExhausted: return "Arena blocks exhausted";
            case ErrorCode::RingBufferFull: return "Ring buffer is full";
            case ErrorCode::RingBufferEmpty: return "Ring buffer is empty";
            case ErrorCode::FlowNotFound: return "Flow state not found";
            case ErrorCode::FlowTableFull: return "Flow table capacity reached";
            case ErrorCode::InvalidArgument: return "Invalid argument provided";
            default: return "Unknown titan error";
        }
    }
};

inline const std::error_category& get_error_category() noexcept {
    static ErrorCategory instance;
    return instance;
}

inline std::error_code make_error_code(ErrorCode e) noexcept {
    return std::error_code(static_cast<int>(e), get_error_category());
}

} // namespace titan

namespace std {
template <>
struct is_error_code_enum<titan::ErrorCode> : std::true_type {};
} // namespace std

namespace titan {

template <typename T, typename E = ErrorCode>
class Result {
private:
    union {
        T value_;
        E error_;
    };
    bool has_value_;

public:
    constexpr Result(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : value_(val), has_value_(true) {}

    constexpr Result(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(val)), has_value_(true) {}

    constexpr Result(E err) noexcept : error_(err), has_value_(false) {
        if (err == ErrorCode::Success) {
            // Force strict mapping
        }
    }

    ~Result() noexcept {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~$E();
        }
    }

    Result(const Result& other) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (static_cast<void*>(&value_)) T(other.value_);
        } else {
            ::new (static_cast<void*>(&error_)) E(other.error_);
        }
    }

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (static_cast<void*>(&value_)) T(std::move(other.value_));
        } else {
            ::new (static_cast<void*>(&error_)) E(std::move(other.error_));
        }
    }

    Result& operator=(const Result& other) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        if (this != &other) {
            this->~Result();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (static_cast<void*>(&value_)) T(other.value_);
            } else {
                ::new (static_cast<void*>(&error_)) E(other.error_);
            }
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            this->~Result();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (static_cast<void*>(&value_)) T(std::move(other.value_));
            } else {
                ::new (static_cast<void*>(&error_)) E(std::move(other.error_));
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

    [[nodiscard]] constexpr E error() const noexcept {
        return error_;
    }
};

class Arena {
public:
    static constexpr size_t kCacheLineSize = 64;
    static constexpr size_t kSlotSize = 2048;
    static constexpr size_t kHugePage2MB = 2 * 1024 * 1024;

private:
    void* base_address_;
    size_t total_size_;
    size_t slot_count_;
    std::atomic<uint64_t> free_bitmap_[64]; // Supports up to 2048 slots with 64x64-bit words
    std::atomic<uint64_t> exhausted_counter_{0};
    uint8_t* slot_base_;

public:
    Arena() noexcept
        : base_address_(nullptr), total_size_(0), slot_count_(0), slot_base_(nullptr) {
        for (auto& word : free_bitmap_) {
            word.store(0, std::memory_order_relaxed);
        }
    }

    explicit Arena(size_t capacity_slots) noexcept
        : base_address_(nullptr), total_size_(0), slot_count_(0), slot_base_(nullptr) {
        for (auto& word : free_bitmap_) {
            word.store(0, std::memory_order_relaxed);
        }
    }

    ~Arena() noexcept {
        if (base_address_ && base_address_ != MAP_FAILED) {
            munmap(base_address_, total_size_);
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept
        : base_address_(other.base_address_),
          total_size_(other.total_size_),
          slot_count_(other.slot_count_),
          slot_base_(other.slot_base_) {
        for (size_t i = 0; i < 64; ++i) {
            free_bitmap_[i].store(other.free_bitmap_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        exhausted_counter_.store(other.exhausted_counter_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.base_address_ = nullptr;
        other.total_size_ = 0;
        other.slot_count_ = 0;
        other.slot_base_ = nullptr;
    }

    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            if (base_address_ && base_address_ != MAP_FAILED) {
                munmap(base_address_, total_size_);
            }
            base_address_ = other.base_address_;
            total_size_ = other.total_size_;
            slot_count_ = other.slot_count_;
            slot_base_ = other.slot_base_;
            for (size_t i = 0; i < 64; ++i) {
                free_bitmap_[i].store(other.free_bitmap_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            exhausted_counter_.store(other.exhausted_counter_.load(std::memory_order_relaxed), std::memory_order_relaxed);

            other.base_address_ = nullptr;
            other.total_size_ = 0;
            other.slot_count_ = 0;
            other.slot_base_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] Result<void, ErrorCode> initialize(size_t num_slots) noexcept {
        if (num_slots == 0 || num_slots > 4096) {
            return ErrorCode::InvalidSize;
        }

        slot_count_ = num_slots;
        size_t raw_size = slot_count_ * kSlotSize;
        
        // Align to 2MB hugepage boundary
        total_size_ = (raw_size + kHugePage2MB - 1) & ~(kHugePage2MB - 1);

        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;
        base_address_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
        
        if (base_address_ == MAP_FAILED) {
            // Fallback to standard pages if hugepages are unavailable
            flags = MAP_PRIVATE | MAP_ANONYMOUS;
            base_address_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (base_address_ == MAP_FAILED) {
                return ErrorCode::OutOfMemory;
            }
        }

        slot_base_ = static_cast<uint8_t*>(base_address_);

        // Initialize bitmaps: all slots available (bits set to 1, or 0 depending on convention. 
        // Let's use 0 = free, 1 = allocated, or vice-versa. Let's use 1 = free, 0 = allocated for atomic test-and-set)
        size_t words_needed = (slot_count_ + 63) / 64;
        for (size_t i = 0; i < words_needed; ++i) {
            uint64_t val = ~uint64_t{0};
            // Mask out excess bits if slot_count_ is not a multiple of 64
            if (i == words_needed - 1) {
                size_t rem = slot_count_ % 64;
                if (rem != 0) {
                    val = (uint64_t{1} << rem) - 1;
                }
            }
            free_bitmap_[i].store(val, std::memory_order_release);
        }
        for (size_t i = words_needed; i < 64; ++i) {
            free_bitmap_[i].store(0, std::memory_order_release);
        }

        return ErrorCode::Success;
    }

    [[nodiscard]] void* allocate_slot() noexcept {
        size_t words_needed = (slot_count_ + 63) / 64;
        for (size_t word_idx = 0; word_idx < words_needed; ++word_idx) {
            uint64_t current = free_bitmap_[word_idx].load(std::memory_order_acquire);
            while (current != 0) {
                unsigned long bit_pos = __builtin_ctzl(current);
                uint64_t mask = uint64_t{1} << bit_pos;
                
                if (free_bitmap_[word_idx].compare_exchange_weak(
                    current, current & ~mask,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                    
                    size_t global_slot = word_idx * 64 + bit_pos;
                    if (global_slot >= slot_count_) {
                        // Revert if out of bounds due to rounding
                        free_bitmap_[word_idx].fetch_or(mask, std::memory_order_release);
                        break;
                    }
                    
                    uint8_t* ptr = slot_base_ + (global_slot * kSlotSize);
                    return static_cast<void*>(ptr);
                }
            }
        }

        exhausted_counter_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    void free_slot(void* ptr) noexcept {
        if (!ptr || !slot_base_) return;
        
        uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);
        if (byte_ptr < slot_base_ || byte_ptr >= slot_base_ + (slot_count_ * kSlotSize)) {
            return;
        }

        size_t offset = byte_ptr - slot_base_;
        size_t global_slot = offset / kSlotSize;
        
        size_t word_idx = global_slot / 64;
        size_t bit_pos = global_slot % 64;
        uint64_t mask = uint64_t{1} << bit_pos;

        free_bitmap_[word_idx].fetch_or(mask, std::memory_order_release);
    }

    [[nodiscard]] uint64_t get_exhausted_count() const noexcept {
        return exhausted_counter_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t slot_size() const noexcept { return kSlotSize; }
    [[nodiscard]] size_t capacity() const noexcept { return slot_count_; }
};

} // namespace titan

#endif // TITAN_ARENA_HPP