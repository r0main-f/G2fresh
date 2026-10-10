// While the emulated G2 runs, the process must not be throttled: macOS's App Nap slows a GUI process it thinks idle
// (a stand-alone app in the background, a host that plays no audio of its own), and the emulator then falls behind
// real time (heard as dropouts). An activity marked latency-critical prevents that; elsewhere this does nothing.
#pragma once

#include <memory>

class NoAppNap {
public:
    NoAppNap();
    ~NoAppNap();
    NoAppNap(const NoAppNap&) = delete;
    NoAppNap& operator=(const NoAppNap&) = delete;

private:
    void* activity_ = nullptr; // the NSProcessInfo activity token (macOS)
};
