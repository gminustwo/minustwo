// SPDX-License-Identifier: GPL-2.0-or-later
#include "mz_vblank.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/thread.h"
#include "mz_graphics.h"
#include "mz_present_trace.h"

namespace MzVblank {
namespace {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;

// The display's vblanks: how many came, when the last one did, the intervals of the last Window
// (ms) and the rate of the display's mode. A changed mode or display starts the intervals over.
std::mutex mutex;
std::condition_variable cv;
std::uint64_t count;
Clock::time_point last_time;
constexpr int Window = 120;
double intervals[Window];
int filled, slot;
double mode_hz;

void Reset(double hz) {
    filled = slot = 0;
    mode_hz = hz;
}

void Note(Clock::time_point now) {
    std::scoped_lock lock{mutex};
    if (count) {
        intervals[slot] = Ms(now - last_time).count();
        slot = (slot + 1) % Window;
        filled = std::min(filled + 1, Window);
    }
    ++count;
    last_time = now;
}

// The divisor of `rate` (mz_graphics.h RefreshDivisor) while the last Window vblanks came steadily,
// each within a fifth of their mean, and at the mode's rate: with VRR the display refreshes when
// the game presents, so its vblanks would follow the game instead of pacing it. 0 otherwise.
// Under the lock.
int SteadyDivisor(int rate, double* hz) {
    if (filled < Window || mode_hz <= 0.0 || Ms(Clock::now() - last_time).count() > 100.0) return 0;
    double sum = 0.0;
    for (const double interval : intervals) sum += interval;
    const double mean = sum / Window;
    for (const double interval : intervals) {
        if (std::abs(interval - mean) > 0.2 * mean) return 0;
    }
    *hz = 1000.0 / mean;
    return std::abs(*hz - mode_hz) <= 1.5 ? MzGraphics::RefreshDivisor(*hz, rate) : 0;
}

#ifdef _WIN32
// D3DKMT (gdi32): a display's vblank interrupt, independent of any swapchain.
struct OpenAdapter {
    WCHAR device_name[32];
    UINT adapter;
    LUID adapter_luid;
    UINT vidpn_source_id;
};
struct WaitVblank {
    UINT adapter;
    UINT device;
    UINT vidpn_source_id;
};
struct CloseAdapter {
    UINT adapter;
};
using OpenFn = LONG(APIENTRY*)(OpenAdapter*);
using WaitFn = LONG(APIENTRY*)(const WaitVblank*);
using CloseFn = LONG(APIENTRY*)(const CloseAdapter*);

double ModeHz(const WCHAR* device) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    return EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
               ? double(mode.dmDisplayFrequency)
               : 0.0;
}

// The display under the window (looked at again every 64 vblanks, with its mode's rate).
void Follow(HWND window, OpenFn open, WaitFn wait, CloseFn close) {
    Common::SetCurrentThreadName("minustwo:Vblank");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HMONITOR monitor = nullptr;
    OpenAdapter adapter{};
    bool opened = false;
    unsigned since_check = 0;
    const auto shut = [&] {
        if (opened) {
            const CloseAdapter request{adapter.adapter};
            close(&request);
            opened = false;
        }
    };
    for (;;) {
        if (!opened || ++since_check >= 64) {
            since_check = 0;
            const HMONITOR now_on = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            if (now_on != monitor || !opened) {
                shut();
                monitor = now_on;
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                adapter = {};
                if (GetMonitorInfoW(monitor, &info)) {
                    lstrcpynW(adapter.device_name, info.szDevice, 32);
                    opened = open(&adapter) == 0;
                }
                std::scoped_lock lock{mutex};
                Reset(opened ? ModeHz(adapter.device_name) : 0.0);
            } else if (const double hz = ModeHz(adapter.device_name); hz != mode_hz) {
                std::scoped_lock lock{mutex};
                Reset(hz);
            }
            if (!opened) {
                Sleep(500);
                continue;
            }
        }
        const WaitVblank event{adapter.adapter, 0, adapter.vidpn_source_id};
        if (wait(&event) != 0) {
            shut();
            Sleep(100);
            continue;
        }
        Note(Clock::now());
        cv.notify_all();
        if (MzPresentTrace::Enabled()) {
            std::printf("PresentTrace: vblank %.3f\n", MzPresentTrace::NowMs());
        }
    }
}
#endif
} // namespace

void Start(void* native_window) {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [native_window] {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
        const auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName")) : nullptr;
        const auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
        const auto close = gdi ? reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
        if (!native_window || !open || !wait || !close) {
            std::printf("VideoOut: the display's vblank is not available; the vblank runs on a timer\n");
            return;
        }
        std::thread([=] { Follow(static_cast<HWND>(native_window), open, wait, close); }).detach();
    });
#else
    (void)native_window;
#endif
}

Pacer::Pacer(int rate_) : rate(rate_) {
    const char* value = std::getenv("MZ_VBLANK_LOCK");
    enabled = !(value && value[0] == '0');
}

bool Pacer::Wait() {
    if (!enabled) return false;
    std::unique_lock lock{mutex};
    double hz = 0.0;
    const int n = SteadyDivisor(rate, &hz);
    if (n != reported && reports < 32) {
        ++reports;
        if (n) {
            std::printf("VideoOut: vblank locked to the display's: %.2f Hz, a tick every %d of its vblanks\n", hz, n);
        } else {
            std::printf("VideoOut: vblank on a timer (the display's refresh rate is not a multiple of %d Hz, "
                        "or its vblanks are not steady)\n", rate);
        }
    }
    reported = n;
    if (!n) {
        divisor = 0;
        return false;
    }
    if (n != divisor) { // (re)locked: from the latest vblank
        divisor = n;
        last = count;
    }
    const std::uint64_t target = last + std::uint64_t(n);
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(Ms(1500.0 * n / mode_hz));
    if (!cv.wait_until(lock, deadline, [&] { return count >= target; })) {
        divisor = 0; // late: this tick on the timer, the next one locks again
        return false;
    }
    // Woken late (the display's vblank came, this thread ran later): the next tick keeps the
    // phase; after a longer stall it starts from now.
    last = count - target < std::uint64_t(n) ? target : count;
    return true;
}
} // namespace MzVblank
