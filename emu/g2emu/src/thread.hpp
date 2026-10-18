// A thread for running the emulated machine: a big stack, and high priority.
// The stack: the dsp56300 JIT compiles on the thread that runs a DSP, and a block it has just compiled runs nested in
// the compiler's call (JitBlockChain::create), about 20 KB of stack per level: a freshly uploaded G2 frame program of
// many small blocks went deeper than a std::thread's 512 KB on macOS.
// The priority: the machine must keep up with the audio. On macOS the user-interactive class keeps the thread on an
// Apple Silicon Mac's performance cores while the computer is busy (at the default class, seven busy processes left
// the plugin silent: the machine fell far behind real time).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif
#endif

namespace g2emu {

class BigStackThread {
public:
    static constexpr std::size_t StackBytes = 64u << 20;  // address space; only the pages touched are used

    BigStackThread() = default;
    explicit BigStackThread(std::function<void()> fn) : fn_(std::make_unique<std::function<void()>>(std::move(fn)))
    {
#ifdef _WIN32
        thread_ = CreateThread(nullptr, StackBytes, [](LPVOID f) -> DWORD {
            (*static_cast<std::function<void()>*>(f))();
            return 0;
        }, fn_.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if(!thread_) throw std::runtime_error("g2emu: cannot start a thread");
        SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
#else
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, StackBytes);
#ifdef __APPLE__
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        started_ = pthread_create(&thread_, &attr, [](void* f) -> void* {
            (*static_cast<std::function<void()>*>(f))();
            return nullptr;
        }, fn_.get()) == 0;
        pthread_attr_destroy(&attr);
        if(!started_) throw std::runtime_error("g2emu: cannot start a thread");
#endif
    }
    BigStackThread(BigStackThread&& o) noexcept : fn_(std::move(o.fn_))
    {
#ifdef _WIN32
        thread_ = o.thread_;
        o.thread_ = nullptr;
#else
        thread_ = o.thread_;
        started_ = o.started_;
        o.started_ = false;
#endif
    }
    BigStackThread& operator=(BigStackThread&&) = delete;
    ~BigStackThread() { join(); }
    void join()
    {
#ifdef _WIN32
        if(thread_)
        {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
#else
        if(started_) pthread_join(thread_, nullptr);
        started_ = false;
#endif
    }

private:
    std::unique_ptr<std::function<void()>> fn_;
#ifdef _WIN32
    HANDLE thread_ = nullptr;
#else
    pthread_t thread_{};
    bool started_ = false;
#endif
};


// The calling thread at the computer's real-time priority, as a host's audio threads are: one that works `busy`
// seconds in every `period`. On macOS a time-constraint thread: it takes a performance core ahead of every ordinary
// thread, the user-interactive ones included (those gave way to other programs' work for milliseconds at a time:
// re/notes/audio-path.md §3.4).
// Returns whether the thread has it now. macOS only: on Windows and Linux this does nothing and returns false (the
// thread keeps what BigStackThread gave it: THREAD_PRIORITY_HIGHEST on Windows, the default policy on Linux, where
// a real-time class needs a privilege a plugin cannot count on). Where macOS refuses, the thread stays as it was:
// nothing else depends on the answer.
inline bool makeThreadRealtime(double period, double busy)
{
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    if(mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0) return false;
    const auto ticks = [&](double s) { return std::uint32_t(s * 1e9 * double(tb.denom) / double(tb.numer)); };
    thread_time_constraint_policy_data_t policy;
    policy.period = ticks(period);
    policy.computation = ticks(busy);
    policy.constraint = ticks(period);
    policy.preemptible = 1;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
    (void)period;
    (void)busy;
    return false;
#endif
}

// A real-time thread that computes for about a second without ever waiting is taken for a runaway by macOS: it loses
// its priority for the next two seconds (XNU's fail-safe, osfmk/kern/priority.c thread_quantum_expire: more than
// max_unsafe_rt_computation, 100 quanta of 10 ms, since the thread last waited demotes it for twice as long). The
// machine's threads do compute without a wait when they are behind (the boot, an upload, after a stall), and beside
// eight busy threads a machine without this fell behind and stayed there, in the one comparison made (re/notes/
// audio-path.md §3.4: provisional). So a real-time thread takes a breath: call this often while working; every
// 50 ms it sleeps for 20 microseconds (0.04 % of the time), and the count starts again. Nothing elsewhere.
class RealtimeBreath {
public:
    void operator()()
    {
#ifdef __APPLE__
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        if(now - last_ < std::chrono::milliseconds(50)) return;
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        last_ = Clock::now();
#endif
    }

private:
#ifdef __APPLE__
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
#endif
};

// A hint to the core that this thread spins (lets the other hardware threads of the core run, saves power).
inline void cpuRelax()
{
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER) && !defined(__clang__)
    __isb(_ARM64_BARRIER_SY);
#else
    __asm__ __volatile__("isb" ::: "memory");
#endif
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    __builtin_ia32_pause();
#elif defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#endif
}

// Waiting for another thread's progress (published in atomics): spin a little, for the common case of a wait of a
// few microseconds, then sleep until the other thread rings. A thread that waits for longer (the machine is ahead
// of the audio and its runner sleeps) then costs no CPU, and leaves its core to the threads that have work.
// ring() after every change a waiter may wait for: it costs a fence and a load while nobody sleeps.
class Doorbell {
public:
    // `timeout`: how long a sleep lasts without a ring, only a safety net (ring() does not miss one); a test makes it
    // long to show that no ring is lost.
    explicit Doorbell(std::chrono::microseconds timeout = std::chrono::milliseconds(2)) : timeout_(timeout) {}

    void ring()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if(sleepers_.load(std::memory_order_relaxed) == 0) return;
        {
            std::lock_guard lock(mutex_);
            ++seq_;
        }
        cv_.notify_all();
    }

    // Returns once ready() is true; spins for about spinMicros first.
    template<typename F> void wait(F ready, int spinMicros = 50)
    {
        using Clock = std::chrono::steady_clock;
        if(ready()) return;
        const auto spinEnd = Clock::now() + std::chrono::microseconds(spinMicros);
        do
        {
            for(int k = 0; k < 64; ++k)
            {
                if(ready()) return;
                cpuRelax();
            }
        } while(Clock::now() < spinEnd);
        std::unique_lock lock(mutex_);
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);  // ready() reads after the count is seen (as ring())
        while(!ready())
        {
            const auto seq = seq_;
            // the timeout only bounds the damage of a missed ring; ring() does not miss one (see above)
            if(!cv_.wait_for(lock, timeout_, [&] { return seq_ != seq; }))
                timeouts_.fetch_add(1, std::memory_order_relaxed);
        }
        sleepers_.fetch_sub(1, std::memory_order_seq_cst);
    }

    // sleeps that ended without a ring (a waiter that waited longer than the timeout, or a lost ring)
    std::uint64_t timeouts() const { return timeouts_.load(std::memory_order_relaxed); }

private:
    alignas(128) std::atomic<int> sleepers_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t seq_ = 0;  // guarded by mutex_
    const std::chrono::microseconds timeout_;
};

} // namespace g2emu
