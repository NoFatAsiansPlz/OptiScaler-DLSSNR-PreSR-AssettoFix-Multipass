#pragma once
#include "Common.h"
#include <cmath>
namespace nr {
class FramePacer {
    Handle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
    double next = 0;
public:
    FramePacer() { if (!timer.value) Check(HRESULT_FROM_WIN32(GetLastError()), "Frame pacing timer"); }
    bool Due(double fps, double now) const { return !fps || now >= next; }
    void Submitted(double fps, double started) { next = fps ? started + 1000.0 / fps : 0; }
    void Wait(HANDLE stop, HANDLE capture = nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)std::ceil((next - ClockMs()) * 10000));
        Check(SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
            "Set frame pacing timer");
        HANDLE events[]{stop, timer.value, capture};
        if (WaitForMultipleObjects(capture ? 3 : 2, events, FALSE, 20) == WAIT_FAILED) // Keep focus/visibility changes responsive at low caps.
            Check(HRESULT_FROM_WIN32(GetLastError()), "Wait for next capture/frame");
    }
};
}
