// SPDX-License-Identifier: GPL-2.0-or-later
// minustwo: MZ_PRESENT_TRACE=<file> — what each presented frame shows and when it reached the
// screen (movie pacing). The presenter samples every game frame it makes at Cols x Rows points
// into <file>; the flip of each frame on its vblank tick, its present and every display vblank
// (Windows, mz_vblank.h) go to the log as "PresentTrace:" lines in steady-clock milliseconds.
// tools/present_report.py matches the samples to a movie's frames. Diagnostic only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace MzPresentTrace {
constexpr std::uint32_t Cols = 48;
constexpr std::uint32_t Rows = 27;

bool Enabled();
double NowMs();
/// One record: frame sequence number, time the frame was made, texel size, then the samples
/// (Rows x Cols texels of `texel_bytes` each, row by row).
void WriteSamples(std::uint64_t seq, double made_ms, std::uint32_t texel_bytes, const void* data,
                  std::size_t size);
} // namespace MzPresentTrace
