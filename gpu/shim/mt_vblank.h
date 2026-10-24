// SPDX-License-Identifier: GPL-2.0-or-later
// minustwo: the vblanks of the display the game window is on (Windows: D3DKMT), and the emulated
// vblank locked to them. On a timer the two clocks drift apart: even on a 120 Hz display a 60 FPS
// frame then lasts 1 or 3 refreshes now and then. Locked, every frame lasts the same.
#pragma once

#include <cstdint>

namespace MtVblank {
/// Follows the display of `native_window` (an HWND) from a thread of its own; once. MT_PRESENT_TRACE
/// logs its vblanks (mt_present_trace.h).
void Start(void* native_window);

/// The emulated vblank at `rate` Hz on the display's own: every n-th display vblank while the
/// display runs at a whole multiple n of the rate and its vblanks come steadily at its mode's rate
/// (not with VRR, not during a mode change). MT_VBLANK_LOCK=0 turns it off.
class Pacer {
public:
    explicit Pacer(int rate);
    /// Waits for this tick's display vblank: true when it came (the tick starts now), false when
    /// the vblank is not locked (the caller's timer paces the tick).
    bool Wait();

private:
    int rate;
    bool enabled;
    int divisor = 0;      ///< display vblanks per tick while locked, 0 on the timer
    std::uint64_t last = 0; ///< the display vblank of the last tick
    int reported = -1, reports = 0; ///< the lock state the log last told, and how often it did
};
} // namespace MtVblank
