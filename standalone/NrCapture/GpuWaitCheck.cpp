#include "Gpu.h"
#include <iostream>
#include <thread>

void GpuWaitCheck() {
    nr::Gpu gpu;
    nr::Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    nr::Handle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
    if (!stop.value || !timer.value) throw std::runtime_error("Wait check handles unavailable.");
    double total = 0;
    for (unsigned i = 0; i < 20; ++i) {
        // Deliberately delay completion without launching capture or graphics windows.
        const auto serial = ++gpu.serial;
        LARGE_INTEGER due{}; due.QuadPart = -30000; // 3 ms, high-resolution timer.
        nr::Check(SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE) ? S_OK :
            HRESULT_FROM_WIN32(GetLastError()), "Wait check timer");
        const auto start = nr::ClockMs();
        std::jthread signal([&] {
            WaitForSingleObject(timer.value, INFINITE);
            gpu.fence->Signal(serial);
        });
        while (!gpu.Complete()) gpu.WaitForCompletion(stop.value);
        total += nr::ClockMs() - start;
    }
    ++gpu.serial; SetEvent(stop.value);
    gpu.WaitForCompletion(stop.value);
    if (gpu.Complete()) throw std::runtime_error("Stop incorrectly completed the GPU fence.");
    nr::Check(gpu.fence->Signal(gpu.serial), "Release stopped test fence");
    ResetEvent(stop.value);
    // Exercise real queue submission after cancellation; Drain has its own event.
    for (unsigned i = 0; i < 10; ++i) {
        gpu.Begin(); gpu.Submit();
        while (!gpu.Complete()) gpu.WaitForCompletion(stop.value);
    }
    if (!gpu.Drain()) throw std::runtime_error("Drain after interrupted wait failed.");
    std::cout << std::format("PASS fence wake: {:.2f} ms average for 3 ms delayed completion; stop cancellation and queue reuse\n", total / 20);
}
