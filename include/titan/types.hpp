#ifndef TITAN_TYPES_HPP
#define TITAN_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <new>

namespace titan {

    // System-wide error codes mapped to prevent exception unwinding in the hot path.
    enum class ErrorCode : uint32_t {
        Success = 0,
        ArenaExhausted,
        RingBufferFull,
        RingBufferEmpty,
        InvalidParameter,
        FlowTableFull,
        FlowNotFound,
        Timeout,
        NotSupported
    };

    // Expected monad for zero-overhead, exception-free error propagation.
    template <typename T>
    class Result {
    private:
        alignas(T) unsigned char storage_[sizeof(T)];
        ErrorCode error_;
        bool has_value_;

    public:
        constexpr Result(const T& val) noexcept 
            : error_(ErrorCode::Success), has_value_(true) {
            ::new (static_cast<void*>(storage_)) T(val);
        }

        constexpr Result(T&& val) noexcept 
            : error_(ErrorCode::Success), has_value_(true) {
            ::new (static_cast<void*>(storage_)) T(std::move(val));
        }

        constexpr Result(ErrorCode err) noexcept 
            : error_(err), has_value_(false) {
            static_assert(err != ErrorCode::Success, "Cannot construct error Result with Success code");
        }

        ~Result() noexcept {
            if (has_value_) {
                reinterpret_cast<T*>(storage_)->~T();
            }
        }

        Result(const Result& other) noexcept 
            : error_(other.error_), has_value_(other.has_value_) {
            if (has_value_) {
                ::new (static_cast<void*>(storage_)) T(*other.ptr());
            }
        }

        Result(Result&& other) noexcept 
            : error_(other.error_), has_value_(other.has_value_) {
            if (has_value_) {
                ::new (static_cast<void*>(storage_)) T(std::move(*other.ptr()));
                other.has_value_ = false;
            }
        }

        Result& operator=(const Result& other) noexcept {
            if (this != &other) {
                if (has_value_) {
                    ptr()->~T();
                }
                error_ = other.error_;
                has_value_ = other.has_value_;
                if (has_value_) {
                    ::new (static_cast<void*>(storage_)) T(*other.ptr());
                }
            }
            return *this;
        }

        Result& operator=(Result&& other) noexcept {
            if (this != &other) {
                if (has_value_) {
                    ptr()->~T();
                }
                error_ = other.error_;
                has_value_ = other.has_value_;
                if (has_value_) {
                    ::new (static_cast<void*>(storage_)) T(std::move(*other.ptr()));
                    other.has_value_ = false;
                }
            }
            return *this;
        }

        [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }
        [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }

        [[nodiscard]] T& value() & noexcept {
            return *ptr();
        }

        [[nodiscard]] const T& value() const & noexcept {
            return *ptr();
        }

        [[nodiscard]] T&& value() && noexcept {
            return std::move(*ptr());
        }

    private:
        [[nodiscard]] T* ptr() noexcept {
            return reinterpret_cast<T*>(storage_);
        }

        [[nodiscard]] const T* ptr() const noexcept {
            return reinterpret_cast<const T*>(storage_);
        }
    };

    // Specialization for void return types to maintain uniform error propagation APIs.
    template <>
    class Result<void> {
    private:
        ErrorCode error_;

    public:
        constexpr Result() noexcept : error_(ErrorCode::Success) {}
        constexpr Result(ErrorCode err) noexcept : error_(err) {}

        ~Result() = default;
        Result(const Result&) = default;
        Result(Result&&) noexcept = default;
        Result& operator=(const Result&) = default;
        Result& operator=(Result&&) noexcept = default;

        [[nodiscard]] constexpr bool has_value() const noexcept { return error_ == ErrorCode::Success; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return error_ == ErrorCode::Success; }
        [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }
    };

    // Constants governing the zero-copy packet memory layout.
    struct PacketLayout {
        static constexpr size_t kSlotSize = 2048;
        static constexpr size_t kCacheLineSize = 64;
        static constexpr size_t kMaxPacketSize = 1536;
        static_assert(kSlotSize % kCacheLineSize == 0, "Slot size must be a multiple of cache line size");
    };

    // Telemetry and diagnostic counters updated via atomic operations in the hot path.
    struct alignas(PacketLayout::kCacheLineSize) TelemetryCounters {
        std::atomic<uint64_t> packets_ingested{0};
        std::atomic<uint64_t> packets_dropped_arena_exhaustion{0};
        std::atomic<uint64_t> packets_dropped_ring_full{0};
        std::atomic<uint64_t> packets_dropped_red{0};
        std::atomic<uint64_t> bytes_processed{0};
    };

} // namespace titan

#endif // TITAN_TYPES_HPP