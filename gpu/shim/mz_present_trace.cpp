// SPDX-License-Identifier: GPL-2.0-or-later
#include "mz_present_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

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
} // namespace MzPresentTrace
