#include <iostream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <cassert>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------
// Mock Implementation of "titan-packet-filter" for testing
// ---------------------------------------------------------
namespace Titan {

enum class Action {
    DROP = 0,
    ALLOW = 1,
    REDIRECT = 2
};

struct Packet {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint16_t payload_len;
    const uint8_t* payload;
};

struct Rule {
    uint32_t src_ip_mask;
    uint32_t src_ip_val;
    uint32_t dst_ip_mask;
    uint32_t dst_ip_val;
    uint16_t src_port_min;
    uint16_t src_port_max;
    uint16_t dst_port_min;
    uint16_t dst_port_max;
    uint8_t  protocol;
    bool     protocol_match;
    Action   action;
};

class PacketFilter {
public:
    static constexpr size_t MAX_RULES = 256;

    PacketFilter() : default_action_(Action::ALLOW), rule_count_(0), active_drops_(0), active_allows_(0) {
        std::memset(rules_, 0, sizeof(rules_));
    }

    ~PacketFilter() = default;

    bool add_rule(const Rule& rule) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (rule_count_ >= MAX_RULES) {
            return false;
        }
        rules_[rule_count_++] = rule;
        return true;
    }

    void clear_rules() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        rule_count_ = 0;
        std::memset(rules_, 0, sizeof(rules_));
    }

    void set_default_action(Action action) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        default_action_ = action;
    }

    Action evaluate(const Packet& pkt) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        for (size_t i = 0; i < rule_count_; ++i) {
            const auto& r = rules_[i];
            
            if ((pkt.src_ip & r.src_ip_mask) != (r.src_ip_val & r.src_ip_mask)) continue;
            if ((pkt.dst_ip & r.dst_ip_mask) != (r.dst_ip_val & r.dst_ip_mask)) continue;
            if (pkt.src_port < r.src_port_min || pkt.src_port > r.src_port_max) continue;
            if (pkt.dst_port < r.dst_port_min || pkt.dst_port > r.dst_port_max) continue;
            if (r.protocol_match && pkt.protocol != r.protocol) continue;

            if (r.action == Action::DROP) {
                active_drops_.fetch_add(1, std::memory_order_relaxed);
            } else if (r.action == Action::ALLOW) {
                active_allows_.fetch_add(1, std::memory_order_relaxed);
            }
            return r.action;
        }

        if (default_action_ == Action::DROP) {
            active_drops_.fetch_add(1, std::memory_order_relaxed);
        } else {
            active_allows_.fetch_add(1, std::memory_order_relaxed);
        }
        return default_action_;
    }

    size_t get_rule_count() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return rule_count_;
    }

    uint64_t get_total_drops() const {
        return active_drops_.load(std::memory_order_relaxed);
    }

    uint64_t get_total_allows() const {
        return active_allows_.load(std::memory_order_relaxed);
    }

    void reset_metrics() {
        active_drops_.store(0, std::memory_order_relaxed);
        active_allows_.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::recursive_mutex mutex_;
    Action default_action_;
    size_t rule_count_;
    Rule rules_[MAX_RULES];
    std::atomic<uint64_t> active_drops_;
    std::atomic<uint64_t> active_allows_;
};

} // namespace Titan

// ---------------------------------------------------------
// Test Suite Implementation
// ---------------------------------------------------------
class TestSuite {
public:
    static void run_all() {
        std::cout << "[----------] Starting TitanPacketFilter Exhaustive Test Suite\n";
        
        test_unit_boundaries();
        test_invariants();
        test_concurrent_stress();
        test_failure_recovery();

        std::cout << "[----------] All tests passed successfully!\n";
    }

private:
    static void test_unit_boundaries() {
        std::cout << "[ RUN      ] TestUnitBoundaries\n";
        Titan::PacketFilter filter;

        // 1. Exact match rules
        Titan::Rule r1 = {
            0xFFFFFFFF, 0xC0A80101, // src_ip: 192.168.1.1
            0xFFFFFFFF, 0x0A000001, // dst_ip: 10.0.0.1
            80, 80,                 // src_port
            80, 80,                 // dst_port
            6, true,                // TCP
            Titan::Action::ALLOW
        };
        assert(filter.add_rule(r1));

        Titan::Packet p1{0xC0A80101, 0x0A000001, 80, 80, 6, 0, nullptr};
        assert(filter.evaluate(p1) == Titan::Action::ALLOW);

        // 2. Boundary port ranges
        Titan::Rule r2 = {
            0, 0, 0, 0,
            1024, 2048,             // src_port range boundary
            0, 65535,
            17, true,               // UDP
            Titan::Action::DROP
        };
        assert(filter.add_rule(r2));

        // Lower boundary
        Titan::Packet p2_low{0, 0, 1024, 5000, 17, 0, nullptr};
        assert(filter.evaluate(p2_low) == Titan::Action::DROP);

        // Upper boundary
        Titan::Packet p2_high{0, 0, 2048, 5000, 17, 0, nullptr};
        assert(filter.evaluate(p2_high) == Titan::Action::DROP);

        // Out of boundary
        Titan::Packet p2_out{0, 0, 2049, 5000, 17, 0, nullptr};
        assert(filter.evaluate(p2_out) == Titan::Action::ALLOW); // Falls through to default ALLOW

        // 3. Subnet Mask Boundaries
        Titan::Rule r3 = {
            0xFFFFFF00, 0xC0A80200, // 192.168.2.0/24
            0, 0,
            0, 65535, 0, 65535,
            0, false,
            Titan::Action::DROP
        };
        assert(filter.add_rule(r3));

        assert(filter.evaluate(Titan::Packet{0xC0A80200, 0, 0, 0, 0, 0, nullptr}) == Titan::Action::DROP);
        assert(filter.evaluate(Titan::Packet{0xC0A802FF, 0, 0, 0, 0, 0, nullptr}) == Titan::Action::DROP);
        assert(filter.evaluate(Titan::Packet{0xC0A80300, 0, 0, 0, 0, 0, nullptr}) == Titan::Action::ALLOW);

        std::cout << "[ PASSED   ] TestUnitBoundaries\n";
    }

    static void test_invariants() {
        std::cout << "[ RUN      ] TestInvariants\n";
        Titan::PacketFilter filter;

        // Invariant 1: Rule capacity cannot exceed MAX_RULES
        for (size_t i = 0; i < Titan::PacketFilter::MAX_RULES; ++i) {
            Titan::Rule r = {0,0,0,0,0,65535,0,65535,0,false,Titan::Action::ALLOW};
            assert(filter.add_rule(r));
        }
        
        // Next rule must fail
        Titan::Rule extra_rule = {0,0,0,0,0,65535,0,65535,0,false,Titan::Action::ALLOW};
        assert(!filter.add_rule(extra_rule));
        assert(filter.get_rule_count() == Titan::PacketFilter::MAX_RULES);

        // Invariant 2: Packet evaluation metrics invariant (Allows + Drops == Total Evaluated)
        filter.clear_rules();
        filter.set_default_action(Titan::Action::ALLOW);
        
        Titan::Rule drop_rule = {0,0,0,0,0,65535,0,65535,0,false,Titan::Action::DROP};
        filter.add_rule(drop_rule);

        for (int i = 0; i < 1000; ++i) {
            Titan::Packet p{static_cast<uint32_t>(i), 0, 0, 0, 0, 0, nullptr};
            filter.evaluate(p);
        }

        uint64_t total_evals = filter.get_total_allows() + filter.get_total_drops();
        assert(total_evals == 1000);

        std::cout << "[ PASSED   ] TestInvariants\n";
    }

    static void test_concurrent_stress() {
        std::cout << "[ RUN      ] TestConcurrentStress (Race Condition & Contention)\n";
        
        Titan::PacketFilter filter;
        std::atomic<bool> start_signal{false};
        std::atomic<int> errors{0};

        constexpr int num_threads = 16;
        constexpr int iterations_per_thread = 5000;

        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        // Spawn threads concurrently mutating rules and evaluating packets
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([t, &filter, &start_signal, &errors]() {
                while (!start_signal.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                std::mt19937 rng(t + 1);
                std::uniform_int_distribution<uint32_t> dist_ip(0, 0xFFFFFFFF);
                std::uniform_int_distribution<uint16_t> dist_port(0, 65535);
                std::uniform_int_distribution<int> dist_op(0, 9);

                for (int i = 0; i < iterations_per_thread; ++i) {
                    int op = dist_op(rng);
                    if (op < 3) {
                        // Mutate rules: Clear & Add
                        filter.clear_rules();
                        Titan::Rule r = {
                            0, 0, 0, 0,
                            dist_port(rng), dist_port(rng),
                            dist_port(rng), dist_port(rng),
                            6, true,