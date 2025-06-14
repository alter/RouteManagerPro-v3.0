// src/service/PerformanceMonitor.h
#pragma once
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <string>
#include <deque>
#include <vector>
#include <algorithm>

class PerformanceMonitor {
public:
    static PerformanceMonitor& Instance() {
        static PerformanceMonitor instance;
        return instance;
    }

    // Timer for measuring operations
    class ScopedTimer {
    public:
        ScopedTimer(const std::string& operation)
            : op(operation), start(std::chrono::high_resolution_clock::now()) {
        }

        ~ScopedTimer() {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            PerformanceMonitor::Instance().RecordOperation(op, duration);
        }

    private:
        std::string op;
        std::chrono::high_resolution_clock::time_point start;
    };

    // Counter for events
    void IncrementCounter(const std::string& name) {
        std::lock_guard<std::mutex> lock(countersMutex);
        counters[name]++;
    }

    // Record operation timing
    void RecordOperation(const std::string& operation, std::chrono::microseconds duration) {
        std::lock_guard<std::mutex> lock(timingsMutex);

        auto& timing = timings[operation];
        timing.count++;
        timing.totalTime += duration;
        timing.lastTime = duration;

        if (duration < timing.minTime || timing.minTime.count() == 0) {
            timing.minTime = duration;
        }
        if (duration > timing.maxTime) {
            timing.maxTime = duration;
        }

        // Keep last N samples for percentile calculations
        timing.recentSamples.push_back(duration);
        if (timing.recentSamples.size() > MAX_SAMPLES) {
            timing.recentSamples.pop_front();
        }
    }

    // Get performance report
    struct PerformanceReport {
        struct OperationStats {
            std::string name;
            uint64_t count;
            std::chrono::microseconds avgTime;
            std::chrono::microseconds minTime;
            std::chrono::microseconds maxTime;
            std::chrono::microseconds p95Time;
        };

        std::vector<OperationStats> operations;
        std::unordered_map<std::string, uint64_t> counters;
        std::chrono::system_clock::time_point reportTime;
    };

    PerformanceReport GetReport() const {
        PerformanceReport report;
        report.reportTime = std::chrono::system_clock::now();

        // Copy counters - need to extract values from atomics
        {
            std::lock_guard<std::mutex> lock(countersMutex);
            for (const auto& [name, value] : counters) {
                report.counters[name] = value.load();
            }
        }

        // Process timings
        {
            std::lock_guard<std::mutex> lock(timingsMutex);
            for (const auto& [op, timing] : timings) {
                PerformanceReport::OperationStats stats;
                stats.name = op;
                stats.count = timing.count;
                stats.minTime = timing.minTime;
                stats.maxTime = timing.maxTime;

                if (timing.count > 0) {
                    stats.avgTime = timing.totalTime / timing.count;
                    stats.p95Time = CalculatePercentile(timing.recentSamples, 95);
                }

                report.operations.push_back(stats);
            }
        }

        return report;
    }

    void Reset() {
        std::lock_guard<std::mutex> lock1(countersMutex);
        std::lock_guard<std::mutex> lock2(timingsMutex);
        counters.clear();
        timings.clear();
    }

private:
    PerformanceMonitor() = default;

    struct TimingInfo {
        uint64_t count = 0;
        std::chrono::microseconds totalTime{ 0 };
        std::chrono::microseconds lastTime{ 0 };
        std::chrono::microseconds minTime{ 0 };
        std::chrono::microseconds maxTime{ 0 };
        std::deque<std::chrono::microseconds> recentSamples;
    };

    static constexpr size_t MAX_SAMPLES = 1000;

    mutable std::mutex countersMutex;
    std::unordered_map<std::string, std::atomic<uint64_t>> counters;

    mutable std::mutex timingsMutex;
    std::unordered_map<std::string, TimingInfo> timings;

    std::chrono::microseconds CalculatePercentile(
        const std::deque<std::chrono::microseconds>& samples, int percentile) const {
        if (samples.empty()) return std::chrono::microseconds{ 0 };

        std::vector<std::chrono::microseconds> sorted(samples.begin(), samples.end());
        std::sort(sorted.begin(), sorted.end());

        size_t index = (sorted.size() * percentile) / 100;
        if (index >= sorted.size()) index = sorted.size() - 1;

        return sorted[index];
    }
};

// Convenience macros
#define PERF_TIMER(operation) PerformanceMonitor::ScopedTimer _timer(operation)
#define PERF_COUNT(counter) PerformanceMonitor::Instance().IncrementCounter(counter)