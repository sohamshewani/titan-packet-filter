#include "engine.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <cstring>
#include <cerrno>
#include <new>

namespace titan {

// Arena Implementation

Arena::Arena(size_t block_size, size_t num_blocks)
    : block_size_(block_size),
      num_blocks_(num_blocks),
      total_size_(block_size * num_blocks),
      base_address_(nullptr),
      free_head_(0),
      exhaustion_count_(0)
{
    // Align block size to 64-byte cache lines
    if (block_size_ % 64 != 0) {
        block_size_ = ((block_size_ + 63) / 64) * 64;
    }
    total_size_ = block_size_ * num_blocks_;

    // Allocate hugepage-backed memory arena using anonymous mmap with hugepage hints
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_HUGETLB
    // Try to allocate with 2MB hugepages if possible, fallback gracefully or use standard large alignment
    mmap_flags |= MAP_HUGETLB;
#endif

    base_address_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (base_address_ == MAP_FAILED) {
        // Fallback without MAP_HUGETLB if hugepages are exhausted or unavailable
        mmap_flags &= ~MAP_HUGETLB;
        base_address_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
        if (base_address_ == MAP_FAILED) {
            base_address_ = nullptr;
            total_size_ = 0;
            num_blocks_ = 0;
            return;
        }
    }

    // Initialize free list: each block points to the next block index
    free_list_.resize(num_blocks_);
    for (size_t i = 0; i < num_blocks_ - 1; ++i) {
        free_list_[i] = i + 1;
    }
    if (num_blocks_ > 0) {
        free_list_[num_blocks_ - 1] = kInvalidIndex;
    }
}

Arena::~Arena() noexcept {
    if (base_address_ != nullptr && base_address_ != MAP_FAILED) {
        munmap(base_address_, total_size_);
        base_address_ = nullptr;
    }
}

Arena::Arena(Arena&& other) noexcept
    : block_size_(other.block_size_),
      num_blocks_(other.num_blocks_),
      total_size_(other.total_size_),
      base_address_(other.base_address_),
      free_list_(std::move(other.free_list_)),
      free_head_(other.free_head_.load(std::memory_order_relaxed)),
      exhaustion_count_(other.exhaustion_count_.load(std::memory_order_relaxed))
{
    other.base_address_ = nullptr;
    other.num_blocks_ = 0;
    other.total_size_ = 0;
    other.free_head_.store(kInvalidIndex, std::memory_order_relaxed);
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        if (base_address_ != nullptr && base_address_ != MAP_FAILED) {
            munmap(base_address_, total_size_);
        }
        block_size_ = other.block_size_;
        num_blocks_ = other.num_blocks_;
        total_size_ = other.total_size_;
        base_address_ = other.base_address_;
        free_list_ = std::move(other.free_list_);
        free_head_.store(other.free_head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        exhaustion_count_.store(other.exhaustion_count_.load(std::memory_order_relaxed), std::memory_order_relaxed);

        other.base_address_ = nullptr;
        other.num_blocks_ = 0;
        other.total_size_ = 0;
        other.free_head_.store(kInvalidIndex, std::memory_order_relaxed);
    }
    return *this;
}

void* Arena::allocate() noexcept {
    size_t current_head = free_head_.load(std::memory_order_acquire);
    while (current_head != kInvalidIndex) {
        size_t next_head = free_list_[current_head];
        if (free_head_.compare_exchange_weak(current_head, next_head,
                                             std::memory_order_release,
                                             std::memory_order_acquire)) {
            // Compute pointer from index
            uint8_t* ptr = static_cast<uint8_t*>(base_address_) + (current_head * block_size_);
            return static_cast<void*>(ptr);
        }
    }
    // Arena Exhaustion
    exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void Arena::deallocate(void* ptr) noexcept {
    if (ptr == nullptr || base_address_ == nullptr) {
        return;
    }
    uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);
    uint8_t* base_ptr = static_cast<uint8_t*>(base_address_);
    ptrdiff_t offset = byte_ptr - base_ptr;
    if (offset < 0 || static_cast<size_t>(offset) >= total_size_) {
        return; // Out of bounds pointer
    }
    size_t index = static_cast<size_t>(offset) / block_size_;

    size_t current_head = free_head_.load(std::memory_order_relaxed);
    do {
        free_list_[index] = current_head;
    } while (!free_head_.compare_exchange_weak(current_head, index,
                                               std::memory_order_release,
                                               std::memory_order_relaxed));
}

// RingBuffer Implementation

RingBuffer::RingBuffer(size_t capacity)
    : capacity_(capacity),
      head_(0),
      tail_(0),
      drop_count_(0)
{
    // Capacity must be a power of 2 for fast bitwise masking
    if ((capacity_ & (capacity_ - 1)) != 0 || capacity_ == 0) {
        size_t cap = 1;
        while (cap < capacity_) {
            cap <<= 1;
        }
        capacity_ = cap == 0 ? 1024 : cap;
    }
    mask_ = capacity_ - 1;
    buffer_ = std::make_unique<Slot[]>(capacity_);
}

bool RingBuffer::enqueue(void* packet_ptr) noexcept {
    uint64_t current_tail = tail_.load(std::memory_order_relaxed);
    uint64_t current_head = head_.load(std::memory_order_acquire);

    if ((current_tail - current_head) >= capacity_) {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        return false; // Ring Saturation / Backpressure
    }

    // Attempt to reserve slot using CAS
    while (!tail_.compare_exchange_weak(current_tail, current_tail + 1,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {
        current_head = head_.load(std::memory_order_acquire);
        if ((current_tail - current_head) >= capacity_) {
            drop_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    size_t index = current_tail & mask_;
    buffer_[index].packet_ptr.store(packet_ptr, std::memory_order_release);
    return true;
}

bool RingBuffer::dequeue(void*& packet_ptr) noexcept {
    uint64_t current_head = head_.load(std::memory_order_relaxed);
    uint64_t current_tail = tail_.load(std::memory_order_acquire);

    if (current_head == current_tail) {
        return false; // Empty
    }

    while (!head_.compare_exchange_weak(current_head, current_head + 1,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {
        current_tail = tail_.load(std::memory_order_acquire);
        if (current_head == current_tail) {
            return false;
        }
    }

    size_t index = current_head & mask_;
    packet_ptr = buffer_[index].packet_ptr.load(std::memory_order_acquire);
    return true;
}

// FlowController Implementation

FlowController::FlowController(size_t table_capacity)
    : capacity_(table_capacity),
      table_(table_capacity),
      red_drop_count_(0)
{
    if ((capacity_ & (capacity_ - 1)) != 0 || capacity_ == 0) {
        size_t cap = 1;
        while (cap < capacity_) {
            cap <<= 1;
        }
        capacity_ = cap == 0 ? 1024 : cap;
    }
    table_.resize(capacity_);
    for (size_t i = 0; i < capacity_; ++i) {
        table_[i].flow_id.store(0, std::memory_order_relaxed);
        table_[i].sequence.store(0, std::memory_order_relaxed);
        table_[i].window_tokens.store(1000, std::memory_order_relaxed);
        table_[i].occupied.store(false, std::memory_order_relaxed);
    }
}

bool FlowController::check_flow(uint64_t flow_id, uint64_t sequence_num) noexcept {
    size_t index = flow_id & (capacity_ - 1);
    size_t start_index = index;

    // Hopscotch / Linear probing hash resolution bounded to kHopRange
    for (size_t i = 0; i < kHopRange; ++i) {
        size_t probe_idx = (start_index + i) & (capacity_ - 1);
        FlowEntry& entry = table_[probe_idx];

        bool is_occupied = entry.occupied.load(std::memory_order_acquire);
        if (is_occupied && entry.flow_id.load(std::memory_order_relaxed) == flow_id) {
            // Existing flow found
            uint64_t last_seq = entry.sequence.load(std::memory_order_relaxed);
            if (sequence_num <= last_seq && last_seq - sequence_num > 10000) {
                // Out of window or replayed packet
                red_drop_count_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            // Update monotonic sequence
            entry.sequence.store(sequence_num, std::memory_order_relaxed);

            // Token bucket rate limiting check
            int64_t tokens = entry.window_tokens.load(std::memory_order_relaxed);
            if (tokens <= 0) {
                // RED / Drop-tail policy enforcement
                red_drop_count_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            entry.window_tokens.fetch_sub(1, std::memory_order_relaxed);
            return true;
        } else if (!is_occupied) {
            // Try to insert new flow atomically
            bool expected_occ = false;
            if (entry.occupied.compare_exchange_strong(expected_occ, true,
                                                         std::memory_order_release,
                                                         std::memory_order_relaxed)) {
                entry.flow_id.store(flow_id, std::memory_order_relaxed);
                entry.sequence.store(sequence_num, std::memory_order_relaxed);
                entry.window_tokens.store(1000, std::memory_order_relaxed);
                return true;
            }
            // If CAS failed, another thread claimed it, re-check current slot
            if (entry.flow_id.load(std::memory_order_relaxed) == flow_id) {
                entry.sequence.store(sequence_num, std::memory_order_relaxed);
                return true;
            }
        }
    }

    // Table collision limit reached (hash exhaustion), trigger drop-tail
    red_drop_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Engine Implementation

Engine::Engine(const EngineConfig& config)
    : config_(config),
      arena_(config.packet_buffer_size, config.ring_capacity),
      ring_(config.ring_capacity),
      flow_controller_(config.flow_table_capacity),
      running_(false),
      epoll_fd_(-1),
      rx_socket_fd_(-1)
{
    metrics_.processed_packets.store(0, std::memory_order_relaxed);
    metrics_.dropped_packets.store(0, std::memory_order_relaxed);
    metrics_.arena_exhaustions.store(0, std::memory_order_relaxed);
    metrics_.ring_saturations.store(0, std::memory_order_relaxed);
}

Engine::~Engine() noexcept {
    stop();
}

Result<void, ErrorCode> Engine::init() noexcept {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        return Result<void, ErrorCode>::err(ErrorCode::SystemError);
    }

    // Setup raw or packet socket for ingestion (simulated via non-blocking eventfd or socketpair for portability)
    rx_socket_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (rx_socket_fd_ < 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
        return Result<void, ErrorCode>::err(ErrorCode::SystemError);
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(rx_socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(rx_socket_fd_);
        close(epoll_fd_);
        rx_socket_fd_ = -1;
        epoll_fd_ = -1;
        return Result<void, ErrorCode>::err(ErrorCode::SystemError);
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = rx_socket_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, rx_socket_fd_, &ev) < 0) {
        close(rx_socket_fd_);
        close(epoll_fd_);
        rx_socket_fd_ = -1;
        epoll_fd_ = -1;
        return Result<void, ErrorCode>::err(ErrorCode::SystemError);
    }

    return Result<void, ErrorCode>::ok();
}

Result<void, ErrorCode> Engine::start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return Result<void, ErrorCode>::err(ErrorCode::AlreadyRunning);
    }

    running_.store(true, std::memory_order_release);

    try {
        worker_thread_ = std::thread(&Engine::reactor_loop, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return Result<void, ErrorCode>::err(ErrorCode::SystemError);
    }

    return Result<void, ErrorCode>::ok();
}

void Engine::stop() noexcept {
    if (!running_.exchange(