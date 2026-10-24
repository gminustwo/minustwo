// SPDX-License-Identifier: GPL-2.0-or-later
// minustwo: graphics options of the in-game menu (Insert / R3+L2), kept in minustwo.ini. The live ones
// apply on the next frame; the startup ones (patches, GPU readback modes) are read by run.py and
// apply through "Apply and restart". The launcher reads and writes the same keys.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MzGraphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, Fullscreen = 2, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {
    "windowed", "borderless", "fullscreen"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {
    "Windowed", "Borderless", "Fullscreen"};

/// Present rate limits; 0 = unlimited.
inline constexpr std::array<int, 8> FpsLimits = {30, 60, 100, 120, 144, 180, 240, 0};

/// Game patch suffixes ("Resolution Patch - <name>"); "native" is no resolution patch.
/// The ones after 900p are the Ultrawide mod's sizes (#202) and the 16:10 and 4:3 ones, built in as beta
/// (scripts/graphics_options.py).
inline constexpr std::array<std::string_view, 23> Resolutions = {
    "native", "480p", "720p", "900p", "1440p", "1800p", "4K",
    "2560x1080", "3440x1440", "3840x1600", "5120x1440", "1920x1200", "2560x1600", "1280x800",
    "1440x900", "1360x768",
    "1280x960", "1440x1080", "1920x1440", "2048x1536", "2560x1920",
    "5120x2160", "8K"};
/// The resolutions to choose from: the built-in ones, then those of the enabled mods' patch files
/// (MZ_EXTRA_RESOLUTIONS, ";"-separated sizes such as "3440x1440"; run.py sets it at every start).
inline std::vector<std::string> ResolutionNames() {
    std::vector<std::string> names(Resolutions.begin(), Resolutions.end());
    const char* extra = std::getenv("MZ_EXTRA_RESOLUTIONS");
    for (std::string_view rest = extra ? extra : ""; !rest.empty();) {
        const std::size_t end = rest.find(';');
        const std::string_view name = rest.substr(0, end);
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.emplace_back(name);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
    }
    return names;
}
/// The size in pixels of a resolution name: the built-in ones (as scripts/graphics_options.py SIZES)
/// or a mod's "<width>x<height>"; anything else is native.
inline constexpr std::array<std::pair<std::string_view, std::pair<int, int>>, 8> ResolutionSizes = {{
    {"native", {1920, 1080}}, {"480p", {854, 480}}, {"720p", {1280, 720}},
    {"900p", {1600, 900}}, {"1440p", {2560, 1440}}, {"1800p", {3200, 1800}}, {"4K", {3840, 2160}},
    {"8K", {7680, 4320}}}};
inline std::pair<int, int> ResolutionSize(std::string_view name) {
    for (const auto& [key, size] : ResolutionSizes) {
        if (key == name) return size;
    }
    const std::size_t x = name.find('x');
    if (x != std::string_view::npos) {
        const int w = std::atoi(std::string(name.substr(0, x)).c_str());
        const int h = std::atoi(std::string(name.substr(x + 1)).c_str());
        if (w > 0 && h > 0) return {w, h};
    }
    return ResolutionSizes[0].second;
}
/// The size the game's 1920x1080 frame is drawn at for a render resolution (the renderer's render
/// scale; GODZILLA has no resolution patches): that height at 16:9, so a 21:9 or 4:3 choice keeps
/// the picture's shape. {0, 0}: the game's own size.
inline std::pair<int, int> RenderScaleSize(std::string_view name) {
    const int height = std::clamp(ResolutionSize(name).second, 180, 4320) & ~1;
    const int width = (height * 16 / 9 + 1) & ~1;
    if (name == "native" || (width == 1920 && height == 1080)) return {0, 0};
    return {width, height};
}
/// Engine frame rates: 60 is the game's own (59.94). The launch takes the 240 FPS patch and the
/// rate is written live (MzSettings::SyncEngineFps); 100, 144 and 180 fit common high-refresh displays.
inline constexpr std::array<int, 6> EngineFps = {60, 100, 120, 144, 180, 240};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Windowed);
}

/// How many `rate` Hz periods one refresh of a display at `hz` is a whole fraction of: 2 for 120 Hz
/// and a 60 Hz rate, 1 for 59.94 Hz; 0 when it is none (144 Hz), so frames could not last the same.
inline int RefreshDivisor(double hz, int rate) {
    if (rate <= 0 || hz < rate * 0.9) return 0;
    const double n = double(long(hz / rate + 0.5));
    return n >= 1 && (hz / n - rate) * (hz / n - rate) <= 0.2 * 0.2 ? int(n) : 0;
}
/// "Match refresh rate" (window.cpp): of a display's refresh rates at the desktop size, the one to
/// switch to when the desktop rate is not a whole multiple of 60 (144 Hz): the highest that is
/// (120 Hz), not above the desktop rate, exactly 120 before 119.88. -1 when there is no need or none.
inline int RefreshFor60(const std::vector<float>& rates, float desktop) {
    if (RefreshDivisor(desktop, 60)) return -1;
    int best = -1;
    for (int i = 0; i < int(rates.size()); ++i) {
        const int n = RefreshDivisor(rates[i], 60);
        if (!n || rates[i] > desktop + 0.5f) continue;
        const auto off = [&](int k) { return std::abs(rates[k] - 60.0f * float(RefreshDivisor(rates[k], 60))); };
        if (best < 0 || n > RefreshDivisor(rates[best], 60) ||
            (n == RefreshDivisor(rates[best], 60) && off(i) < off(best))) {
            best = i;
        }
    }
    return best;
}
/// The primary display, the default when the launcher saved no monitor choice. -1 keeps it distinct
/// from an explicit index 0, which names the first display SDL3 lists even when it is not the primary.
inline constexpr int PrimaryMonitor = -1;
/// The display the window opens on: an index into the list window.cpp reads from SDL3, or
/// PrimaryMonitor. The value comes from the launcher (MZ_MONITOR), so it is untrusted text:
/// empty, negative, non-numeric or too large for an int falls back to PrimaryMonitor.
inline int ParseMonitor(std::string_view text) {
    if (text.empty()) return PrimaryMonitor;
    int index = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return PrimaryMonitor;
        if (index > (std::numeric_limits<int>::max() - (digit - '0')) / 10) return PrimaryMonitor;
        index = index * 10 + (digit - '0');
    }
    return index;
}
/// A listed limit, else unlimited.
inline int ParseFpsLimit(int value) {
    return IndexOf(FpsLimits, value, -1) >= 0 ? value : 0;
}
/// Index into ResolutionNames(); unknown text is native.
inline int ParseResolution(std::string_view text) {
    const std::vector<std::string> names = ResolutionNames();
    const auto found = std::find(names.begin(), names.end(), text);
    return found == names.end() ? 0 : int(found - names.begin());
}
/// A listed ceiling, else 120 (the default patch).
inline int ParseEngineFps(int value) {
    return IndexOf(EngineFps, value, -1) >= 0 ? value : 60;
}

/// Anisotropic filtering forced on the game's mipmapped textures; 0 = the game's own. Samplers
/// are cached, so it applies on restart.
inline constexpr std::array<int, 5> Anisotropies = {0, 2, 4, 8, 16};
inline int ParseAnisotropy(int value) {
    return IndexOf(Anisotropies, value, -1) >= 0 ? value : 0;
}

/// Frames the GPU command thread may queue ahead of the GPU; 0 = unbounded.
inline constexpr std::array<int, 3> FramesQueued = {1, 2, 0};
inline int ParseFramesQueued(int value) {
    return IndexOf(FramesQueued, value, -1) >= 0 ? value : 1;
}
/// RCAS strength in percent; invalid values fall back to 75.
inline int ParseRcasStrength(int value) {
    return value >= 0 && value <= 100 ? value : 75;
}
/// FSR RCAS attenuation for a strength: 0 is the sharpest, 2 the softest.
inline float RcasAttenuation(int strength) {
    return 2.0f * (1.0f - float(ParseRcasStrength(strength)) / 100.0f);
}
/// Whether the FSR pass runs: FSR 1 when the window is larger than the game's image, RCAS
/// alone when they are the same size. A smaller window is left to the regular scaling.
inline bool FsrPassRuns(bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool upscale = in_w < out_w && in_h < out_h;
    const bool same = in_w == out_w && in_h == out_h;
    return (fsr1 && upscale) || (rcas && same);
}

/// What the present limit really allows: the engine ceiling caps it.
inline int EffectiveFps(int limit, int engine_fps) {
    return limit == 0 || limit > engine_fps ? engine_fps : limit;
}

/// The present thread's frame limit: never above the engine rate, since the engine steps a fixed
/// time per frame and faster flips speed the game up. 0 stays vblank paced while the vblank is
/// the engine rate; a live rate below it (the vblank runs at the highest one) caps it.
inline int PresentLimit(int limit, int engine_fps, int vblank_hz) {
    return limit == 0 && vblank_hz <= engine_fps ? 0 : EffectiveFps(limit, engine_fps);
}

/// R3 + L1 + R1 / F9: the engine rate after `current`, up to `highest` (this launch's patch);
/// back to the first after the last.
inline int NextEngineFps(int current, int highest) {
    for (const int fps : EngineFps) {
        if (fps > current && fps <= highest) {
            return fps;
        }
    }
    return EngineFps[0];
}

} // namespace MzGraphics
