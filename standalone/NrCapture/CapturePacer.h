#pragma once
#include "FramePacer.h"
#include <atomic>
#include <stop_token>
namespace nr {
// Pace acquisition, not just NR. Keep the last DXGI frame owned during the wait
// so Windows can accumulate updates instead of copying every game present.
class CapturePacer {
    FramePacer timer;
    Handle changed{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<double> target;
    double applied = -1, lastStarted = 0;
public:
    explicit CapturePacer(double fps = 0) : target(fps) {
        if (!changed.value) Check(HRESULT_FROM_WIN32(GetLastError()), "Capture pacing event");
    }
    void SetRate(double fps) {
        if (target.exchange(fps) != fps) SetEvent(changed.value);
    }
    bool Wait(std::stop_token stop) {
        std::stop_callback wake(stop, [this] { SetEvent(changed.value); });
        while (!stop.stop_requested()) {
            const double fps = target.load();
            if (fps != applied) { applied = fps; timer.Submitted(fps, lastStarted); }
            if (timer.Due(fps, ClockMs())) return true;
            timer.Wait(changed.value);
        }
        return false;
    }
    void Submitted(double started) {
        lastStarted = started;
        timer.Submitted(applied, started);
    }
};
}
