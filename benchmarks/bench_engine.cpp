#include <chrono>
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cstdint>

// Mocking the 'titan-packet-filter' interface for self-contained compilation and benchmarking.
// In a real integration, this would include the actual titan-packet-filter headers.
namespace titan {
    struct Packet {
        uint32_t src_ip;
        uint32_t dst_ip;
        uint16_t src_port;
        uint16_t dst_port;
        uint8_t  protocol;
        const uint8_t* payload;
        size_t length;
    };

    enum class FilterResult {
        ALLOW,
        DROP,
        REDIRECT
    };

    class PacketFilter {
    public:
        PacketFilter() = default;
        ~PacketFilter() = default;

        // Simulate filter rule evaluation
        FilterResult evaluate(const Packet& pkt) {
            // Dummy logic representing rule matching overhead
            if (pkt.src_ip == 0xC0A80101 && pkt.dst_port == 80) {
                return FilterResult::DROP;
            }
            if (pkt.protocol == 6 && pkt.length > 1500) {
                return FilterResult::REDIRECT;
            }
            return FilterResult::ALLOW;
        }
    };
}

int main() {
    constexpr size_t WARMUP_ITERATIONS = 100000;
    constexpr size_t BENCHMARK_ITERATIONS = 5000000;

    titan::PacketFilter filter;
    
    // Prepare test packet
    uint8_t dummy_payload[64] = {0};
    titan::Packet pkt{
        .src_ip = 0xC0A80105,
        .dst_ip = 0x08080808,
        .src_port = 12345,
        .dst_port = 443,
        .protocol = 6,
        .payload = dummy_payload,
        .length = sizeof(dummy_payload)
    };

    // Warmup phase to stabilize CPU frequency and cache state
    for (size_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        volatile auto res = filter.evaluate(pkt);
        (void)res;
    }

    // Latency collection vector
    std::vector<uint64_t> latencies;
    latencies.reserve(BENCHMARK_ITERATIONS);

    // Benchmark loop measuring individual operation latencies via high-resolution clock
    auto total_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < BENCHMARK_ITERATIONS; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        
        volatile auto res = filter.evaluate(pkt);
        (void)res;

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        latencies.push_back(duration);
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration_sec = std::chrono::duration<double>(total_end - total_start).count();

    // Compute throughput
    double ops_sec = static_cast<double>(BENCHMARK_ITERATIONS) / total_duration_sec;

    // Compute percentiles
    std::sort(latencies.begin(), latencies.end());
    uint64_t p50 = latencies[BENCHMARK_ITERATIONS * 50 / 100];
    uint64_t p90 = latencies[BENCHMARK_ITERATIONS * 90 / 100];
    uint64_t p99 = latencies[BENCHMARK_ITERATIONS * 99 / 100];
    uint64_t p999 = latencies[BENCHMARK_ITERATIONS * 999 / 1000];
    
    // Calculate average latency
    uint64_t total_latency = std::accumulate(latencies.begin(), latencies.end(), uint64_t{0});
    double avg_latency = static_cast<double>(total_latency) / BENCHMARK_ITERATIONS;

    // Output formatted tabular metrics
    std::cout << "\n=========================================================\n";
    std::cout << "         TITAN-PACKET-FILTER MICROBENCHMARK RESULTS      \n";
    std::cout << "=========================================================\n";
    std::cout << "Metric                  | Value                          \n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Total Iterations        | " << BENCHMARK_ITERATIONS << "\n";
    std::cout << "Total Duration (s)      | " << total_duration_sec << " s\n";
    std::cout << "Throughput (ops/sec)    | " << ops_sec << " ops/sec\n";
    std::cout << "Average Latency         | " << avg_latency << " ns\n";
    std::cout << "P50 Latency             | " << p50 << " ns\n";
    std::cout << "P90 Latency             | " << p90 << " ns\n";
    std::cout << "P99 Latency             | " << p99 << " ns\n";
    std::cout << "P99.9 Latency           | " << p999 << " ns\n";
    std::cout << "=========================================================\n";

    return 0;
}