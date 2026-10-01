#pragma once

// Main-thread wall time per call of the operations that can stall every output (TD-35):
// source creation and removal, decoder repositioning, the audio sync and the per-frame
// source update. The stress harness and the bench report p50/p99/max per operation.
// Header-only; recording is two clock reads and a push_back, and calls made on any
// thread other than the one markMainThread() named (decode or open workers, unit tests
// that never mark one) are not recorded.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <map>
#include <process.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace quadmapper {

class MainThreadTimings {
public:
    struct Summary {
        std::string op;
        size_t count = 0;
        double p50Ms = 0.0;
        double p99Ms = 0.0;
        double maxMs = 0.0;
    };

    // The calling thread is the render thread from now on.
    static void markMainThread() { state().mainThread.store(std::this_thread::get_id()); }
    static bool onMainThread() { return state().mainThread.load() == std::this_thread::get_id(); }

    static void record(const char* op, double seconds) {
        if (!onMainThread()) return;
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::vector<double>& samples = s.samples[op];
        if (samples.size() < MAX_SAMPLES) samples.push_back(seconds);
    }

    static void reset() {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.samples.clear();
    }

    static std::vector<Summary> summarize() {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::vector<Summary> out;
        for (const auto& [op, recorded] : s.samples) {
            if (recorded.empty()) continue;
            std::vector<double> sorted = recorded;
            std::sort(sorted.begin(), sorted.end());
            auto at = [&sorted](double q) {
                const size_t i = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1) + 0.5);
                return sorted[std::min(i, sorted.size() - 1)] * 1000.0;
            };
            out.push_back({op, sorted.size(), at(0.50), at(0.99), sorted.back() * 1000.0});
        }
        return out;
    }

    // When QM_TIMINGS_DUMP_DIR names a directory, append every sample recorded so far
    // ("op ms" per line) to this process's file there, so a measurement run can pool
    // percentiles across processes and runs.
    static void dumpSamples() {
        const char* dir = std::getenv("QM_TIMINGS_DUMP_DIR");
        if (!dir || !*dir) return;
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::ofstream out(std::string(dir) + "/timings-" + std::to_string(_getpid()) + ".txt", std::ios::app);
        for (const auto& [op, recorded] : s.samples) {
            for (double seconds : recorded) out << op << ' ' << seconds * 1000.0 << '\n';
        }
    }

    // One line per operation: "op: n=.. p50=.. ms p99=.. ms max=.. ms".
    static std::string report() {
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(2);
        for (const Summary& s : summarize()) {
            text << s.op << ": n=" << s.count << " p50=" << s.p50Ms << " ms p99=" << s.p99Ms
                 << " ms max=" << s.maxMs << " ms\n";
        }
        return text.str();
    }

private:
    static constexpr size_t MAX_SAMPLES = size_t(1) << 18;

    struct State {
        std::atomic<std::thread::id> mainThread{};
        std::mutex mutex;
        std::map<std::string, std::vector<double>> samples;
    };
    static State& state() {
        static State s;
        return s;
    }
};

// Records the scope's wall time under `op` (a string literal) when it runs on the main thread.
class ScopedMainThreadTiming {
public:
    explicit ScopedMainThreadTiming(const char* op)
        : op_(op), start_(std::chrono::steady_clock::now()) {}
    ~ScopedMainThreadTiming() {
        MainThreadTimings::record(
            op_, std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count());
    }
    ScopedMainThreadTiming(const ScopedMainThreadTiming&) = delete;
    ScopedMainThreadTiming& operator=(const ScopedMainThreadTiming&) = delete;

private:
    const char* op_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace quadmapper
