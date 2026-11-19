#pragma once
// SpectroCraft - a small parallel-for for the sample loops of the edits and
// the undo history (whole-file gain, fades, EQ, envelope, undo swaps...).
//
// The work is split into contiguous chunks, one per thread; the calling thread
// takes the first chunk itself and waits for the others. Small ranges run on
// the calling thread alone, so short edits pay nothing. An exception thrown in
// a chunk (std::bad_alloc) is rethrown on the calling thread once every chunk
// has finished.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <thread>
#include <vector>

namespace sc::par
{
/** A limit set by ScopedWorkers (0 = none). */
inline std::atomic<int>& workerOverride()
{
    static std::atomic<int> v { 0 };
    return v;
}

/** Threads a parallel loop may use: the hardware threads, at most 16. */
inline int workers()
{
    if (const int o = workerOverride().load(); o > 0)
        return o;
    static const int n = std::clamp ((int) std::thread::hardware_concurrency(), 1, 16);
    return n;
}

/** Limits the loops to `n` threads while it lives (the tests compare one
    thread with many: the results must be identical). */
struct ScopedWorkers
{
    explicit ScopedWorkers (int n) : prev (workerOverride().exchange (std::max (1, n))) {}
    ~ScopedWorkers() { workerOverride().store (prev); }
    ScopedWorkers (const ScopedWorkers&) = delete;
    ScopedWorkers& operator= (const ScopedWorkers&) = delete;
private:
    int prev;
};

/** Below this many items per thread a loop is not split (thread start-up
    costs tens of microseconds; a chunk should be well above that). */
constexpr int64_t kMinPerTask = 1 << 18;   // 262144 samples, about 1 MB of floats

/** Runs fn (begin, end) over [0, n) in contiguous chunks on up to workers()
    threads. Chunks never overlap; fn must only touch its own range (or
    per-chunk state). Blocks until every chunk is done. */
inline void forRange (int64_t n, const std::function<void (int64_t, int64_t)>& fn,
                      int64_t minPerTask = kMinPerTask)
{
    if (n <= 0) return;
    const int64_t maxTasks = std::max<int64_t> (1, n / std::max<int64_t> (1, minPerTask));
    const int tasks = (int) std::min<int64_t> (workers(), maxTasks);
    if (tasks <= 1)
    {
        fn (0, n);
        return;
    }
    const int64_t step = (n + tasks - 1) / tasks;
    std::vector<std::exception_ptr> errors ((size_t) tasks);
    std::vector<std::thread> threads;
    threads.reserve ((size_t) tasks - 1);
    auto run = [&] (int t)
    {
        const int64_t b = (int64_t) t * step, e = std::min (n, b + step);
        if (b >= e) return;
        try { fn (b, e); }
        catch (...) { errors[(size_t) t] = std::current_exception(); }
    };
    for (int t = 1; t < tasks; ++t)
        threads.emplace_back (run, t);
    run (0);
    for (auto& th : threads)
        th.join();
    for (auto& err : errors)
        if (err) std::rethrow_exception (err);
}

/** Copies n floats with forRange (a large copy is limited by one core's
    memory bandwidth; several cores move it faster). */
inline void copy (const float* src, float* dst, int64_t n)
{
    forRange (n, [src, dst] (int64_t b, int64_t e) { std::copy (src + b, src + e, dst + b); });
}

/** Swaps two ranges of n floats with forRange (undo / redo of an edit). */
inline void swapRanges (float* a, float* b, int64_t n)
{
    forRange (n, [a, b] (int64_t s, int64_t e) { std::swap_ranges (a + s, a + e, b + s); });
}
} // namespace sc::par
