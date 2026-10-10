// A thread for running the emulated machine: a big stack, and high priority.
// The stack: the dsp56300 JIT compiles on the thread that runs a DSP, and a block it has just compiled runs nested in
// the compiler's call (JitBlockChain::create), about 20 KB of stack per level: a freshly uploaded G2 frame program of
// many small blocks went deeper than a std::thread's 512 KB on macOS.
// The priority: the machine must keep up with the audio. On macOS the user-interactive class keeps the thread on an
// Apple Silicon Mac's performance cores while the computer is busy (at the default class, seven busy processes left
// the plugin silent: the machine fell far behind real time).
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#ifdef __APPLE__
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


} // namespace g2emu
