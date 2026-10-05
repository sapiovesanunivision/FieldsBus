// Fixed-period cycle timer (internal helper; include it from .cpp files, not from public API headers).
//  Windows: a periodic waitable timer. Prefers CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
//           (Windows 10 1803+: sub-ms accuracy without timeBeginPeriod, unaffected by
//           Windows 11 timer-resolution throttling); falls back to a normal waitable timer.
//  Other:   steady_clock + sleep_until (resync instead of bursting when late).
#pragma once

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// synchapi.h only defines it for _WIN32_WINNT >= Windows 10 RS4; the project targets 0x0601.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace softeip {

class PeriodicTimer {
public:
    explicit PeriodicTimer(std::chrono::milliseconds period)
        : period_(period), next_(std::chrono::steady_clock::now())
    {
#ifdef _WIN32
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highResolution_ = timer_ != nullptr;
        if (!timer_) // older Windows rejects the flag
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(period.count()) * 10000; // relative, 100 ns units
            if (!SetWaitableTimer(timer_, &due, static_cast<LONG>(period.count()), nullptr, nullptr, FALSE)) {
                CloseHandle(timer_);
                timer_ = nullptr;
            }
        }
#endif
    }
    ~PeriodicTimer()
    {
#ifdef _WIN32
        if (timer_)
            CloseHandle(timer_);
#endif
    }
    PeriodicTimer(const PeriodicTimer&) = delete;
    PeriodicTimer& operator=(const PeriodicTimer&) = delete;

    // Blocks until the next period boundary. A missed period is not caught up (no burst).
    void wait()
    {
#ifdef _WIN32
        if (timer_) {
            WaitForSingleObject(timer_, INFINITE);
            return;
        }
#endif
        next_ += period_;
        const auto now = std::chrono::steady_clock::now();
        if (next_ <= now)
            next_ = now + period_;
        std::this_thread::sleep_until(next_);
    }

    const char* kind() const
    {
#ifdef _WIN32
        if (timer_)
            return highResolution_ ? "high-resolution waitable timer" : "waitable timer";
#endif
        return "steady_clock sleep_until";
    }

private:
    std::chrono::milliseconds period_;
    std::chrono::steady_clock::time_point next_;
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    bool highResolution_ = false;
#endif
};

} // namespace softeip
