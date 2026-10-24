// SPDX-License-Identifier: GPL-2.0-or-later
#include "mz_present_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace MzPresentTrace {
namespace {
const char* Path() {
    static const char* path = [] {
        const char* v = std::getenv("MZ_PRESENT_TRACE");
        return v && v[0] ? v : nullptr;
    }();
    return path;
}

std::mutex file_mutex;
std::FILE* file;

#ifdef _WIN32
// D3DKMT (gdi32): waits for a display's vblank interrupt, independent of any swapchain.
struct OpenAdapterFromGdiDisplayName {
    WCHAR device_name[32];
    UINT adapter;
    LUID adapter_luid;
    UINT vidpn_source_id;
};
struct WaitForVerticalBlankEvent {
    UINT adapter;
    UINT device;
    UINT vidpn_source_id;
};
using OpenAdapterFn = LONG(APIENTRY*)(OpenAdapterFromGdiDisplayName*);
using WaitVblankFn = LONG(APIENTRY*)(const WaitForVerticalBlankEvent*);
#endif
} // namespace

bool Enabled() {
    return Path() != nullptr;
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void WriteSamples(std::uint64_t seq, double made_ms, std::uint32_t texel_bytes, const void* data,
                  std::size_t size) {
    std::scoped_lock lock{file_mutex};
    if (!file) {
        file = std::fopen(Path(), "wb");
        if (!file) {
            return;
        }
    }
    std::fwrite(&seq, sizeof(seq), 1, file);
    std::fwrite(&made_ms, sizeof(made_ms), 1, file);
    std::fwrite(&texel_bytes, sizeof(texel_bytes), 1, file);
    std::fwrite(data, 1, size, file);
    std::fflush(file);
}

void StartVblankLog() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        const HMODULE gdi = GetModuleHandleW(L"gdi32.dll") ? GetModuleHandleW(L"gdi32.dll")
                                                            : LoadLibraryW(L"gdi32.dll");
        const auto open = gdi ? reinterpret_cast<OpenAdapterFn>(
                                    GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"))
                              : nullptr;
        const auto wait = gdi ? reinterpret_cast<WaitVblankFn>(
                                    GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"))
                              : nullptr;
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        OpenAdapterFromGdiDisplayName adapter{};
        if (!open || !wait || !GetMonitorInfoW(monitor, &info)) {
            std::printf("PresentTrace: no vblank source\n");
            return;
        }
        lstrcpynW(adapter.device_name, info.szDevice, 32);
        if (open(&adapter) != 0) {
            std::printf("PresentTrace: no vblank source (adapter)\n");
            return;
        }
        std::thread([wait, adapter] {
            const WaitForVerticalBlankEvent event{adapter.adapter, 0, adapter.vidpn_source_id};
            while (wait(&event) == 0) {
                std::printf("PresentTrace: vblank %.3f\n", NowMs());
            }
            std::printf("PresentTrace: vblank wait failed\n");
        }).detach();
    });
#endif
}
} // namespace MzPresentTrace
