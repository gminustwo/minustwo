// SPDX-License-Identifier: GPL-2.0-or-later
#include "mt_overlay.h"
#include "mt_ui_font.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "mt_settings.h"
#include "mt_graphics.h"
#include "mt_perf_stats.h"

extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "../mtgpu.h"       // mtgpu_toast_trophy, mtgpu_trophy_setup
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderdoc.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global mt_font_ttf\n"
    "mt_font_ttf:\n"
    ".incbin \"" MT_FONT_PATH "\"\n"
    ".global mt_font_ttf_end\n"
    "mt_font_ttf_end:\n"
    ".text\n");
#elif defined(__APPLE__)
asm(".section __TEXT,__const\n"
    ".p2align 4\n"
    ".private_extern _mt_font_ttf\n"
    ".globl _mt_font_ttf\n"
    "_mt_font_ttf:\n"
    ".incbin \"" MT_FONT_PATH "\"\n"
    ".private_extern _mt_font_ttf_end\n"
    ".globl _mt_font_ttf_end\n"
    "_mt_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden mt_font_ttf\n"
    ".global mt_font_ttf\n"
    "mt_font_ttf:\n"
    ".incbin \"" MT_FONT_PATH "\"\n"
    ".hidden mt_font_ttf_end\n"
    ".global mt_font_ttf_end\n"
    "mt_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char mt_font_ttf[];
extern "C" const unsigned char mt_font_ttf_end[];

namespace MtOverlay {

namespace {
using MtUi::Tr;

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool r3_down = false, l2_down = false, r2_down = false;
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
std::atomic<u32> prefetch_done{0}, prefetch_total{0};
std::atomic<s64> prefetch_until{0}; // "ready" stays this long after the last program
// Shader translations: in progress, and started since the note last appeared.
std::atomic<u32> compile_pending{0}, compile_count{0};
std::atomic<s64> compile_until{0};
// A short message in a corner (saved screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::string choice_preview_label;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<MtPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

// GODZILLA's look, the launcher's (launcher/minustwo_launcher.py): near black, the atomic blue of
// its menus and HUD, the red of the ゴジラ in its logo.
namespace Theme {
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
constexpr ImVec4 Alpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, c.w * a); }
constexpr ImVec4 Bg = Rgb(6, 8, 12), Panel = Rgb(11, 15, 21), Card = Rgb(18, 26, 36), Line = Rgb(29, 40, 54);
constexpr ImVec4 Raised = Rgb(26, 38, 53); // a card under the mouse
constexpr ImVec4 Text = Rgb(230, 237, 245), Soft = Rgb(190, 201, 214), Muted = Rgb(127, 141, 160);
constexpr ImVec4 Accent = Rgb(22, 128, 216), AccentHi = Rgb(76, 180, 255), Red = Rgb(211, 32, 42);
constexpr ImVec4 Warn = Rgb(240, 166, 64);
ImU32 U32(const ImVec4& c, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(Alpha(c, a)); }
} // namespace Theme
// The launcher's fonts, read from the system's fonts where installed (none is bundled): Impact for
// headings, Bahnschrift for the tabs, Yu Gothic for the ゴジラ. Null: DejaVu Sans instead.
ImFont* heading_font = nullptr;
ImFont* nav_font = nullptr;
ImFont* kana_font = nullptr;
constexpr int kMenuTabs = 5;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        MtSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

void UiText(const char* text) { ImGui::TextUnformatted(Tr(text)); }
void DisabledText(const char* text) { ImGui::TextDisabled("%s", Tr(text)); }
// Text in the heading font (DejaVu Sans for translated text, which Impact has no glyphs for).
void PushHeading(float size) { ImGui::PushFont(MtUi::chinese ? nullptr : heading_font, size); }

std::string Upper(const char* text) {
    std::string upper = text;
    if (!MtUi::chinese) {
        for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    return upper;
}

// A section: its name in capitals, atomic blue, over a thin rule (the launcher's sections).
void Separator(const char* text) {
    const float u = base_scale;
    ImGui::Dummy(ImVec2(0.0f, 6.0f * u));
    PushHeading(22.0f);
    ImGui::TextColored(Theme::AccentHi, "%s", Upper(Tr(text)).c_str());
    ImGui::PopFont();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y - 1.0f * u),
                                        ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y - 1.0f * u),
                                        Theme::U32(Theme::Line), 1.0f * u);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * u));
}
bool Button(const char* text) { return ImGui::Button(Tr(text)); }

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(Tr(label), &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    std::vector<const char*> translated;
    translated.reserve(count);
    for (int i = 0; i < count; ++i) translated.push_back(Tr(labels[i]));
    if (ImGui::Combo(label, &current, translated.data(), count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text follow MT_GLYPHS (run.py sets xbox or nintendo when
// the glyph feature is on); unset is PlayStation. Read once: the environment is fixed for a run.
static const char* PadText(const char* playstation, const char* xbox, const char* nintendo) {
    static const char* style = std::getenv("MT_GLYPHS");
    if (!style) return playstation;
    return !std::strcmp(style, "xbox") ? xbox : !std::strcmp(style, "nintendo") ? nintendo : playstation;
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = Tr(text);
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    UiText(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    DisabledText(text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case MtSettings::UpscalerFsr3: return "FSR 3.1";
    case MtSettings::UpscalerFsr4: return "FSR 4";
    case MtSettings::UpscalerFsr411: return "FSR 4.1.1";
    case MtSettings::UpscalerTaa: return "TAA";
    case MtSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = MtPerfStats;
    const auto& s = MtSettings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

void ImageTab() {
    namespace G = MtGraphics;
    auto& s = MtSettings::Get();
    Separator("Display");
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        CheckRow("Match refresh rate", s.match_refresh,
                 "Borderless and fullscreen: a display whose refresh rate is not a multiple of 60 (144 Hz) "
                 "runs at one that is (120 Hz) while the game runs, so every frame lasts the same.");
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    Separator("Sharpening");
    if (BeginRows("##scaling")) {
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "No effect: the game's image is smaller than the window."
                                             : "Sharpens the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    Separator("Textures (applied on restart)");
    if (BeginRows("##textures")) {
        static const char* const anisotropy_labels[] = {"Game default", "2x", "4x", "8x", "16x"};
        ChoiceRow("Anisotropic filtering", s.anisotropy, anisotropy_labels, G::Anisotropies.data(),
                  int(G::Anisotropies.size()),
                  "Sharper floors and walls seen at an angle. Little cost on most GPUs.");
        ImGui::EndTable();
    }
    DisabledText("F12 saves a screenshot to the user\\screenshots folder.");
    Separator("Information");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Game image", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window", text);
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = MtGraphics;
    auto& s = MtSettings::Get();
    Separator("Live");
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"30", "60", "100", "120", "144", "180", "240", "Unlimited"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. GODZILLA itself runs at up to 60 frames per second.");
        const int effective = G::EffectiveFps(s.fps_limit, MtSettings::EngineFpsNow());
        if (s.fps_limit == 0 || effective < s.fps_limit) {
            char text[48];
            std::snprintf(text, sizeof(text), Tr("%d FPS (engine frame rate)"), effective);
            InfoRow("Effective limit", text);
        }
        if (s.applied_engine_fps <= 60 && s.engine_fps > 60) {
            InfoRow("Engine frame rate", "60 FPS: the frame rate patch is not available for this game build");
        }
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        ImGui::EndTable();
    }
    Separator("Applied on restart");
    if (BeginRows("##restart")) {
        // The built-in sizes, then those of the enabled mods' patch files (ultrawide, for one).
        // The same labels as the launcher (graphics_options.resolution_label): name, exact size, aspect ratio.
        static const char* const builtin_labels[] = {
            "Native · 1920 × 1080 · 16:9", "480p · 854 × 480 · 16:9",
            "720p · 1280 × 720 · 16:9", "900p · 1600 × 900 · 16:9",
            "1440p · 2560 × 1440 · 16:9",
            "1800p · 3200 × 1800 · 16:9", "4K · 3840 × 2160 · 16:9",
            "2560 × 1080 · 21:9 (beta)", "3440 × 1440 · 21:9",
            "3840 × 1600 · 21:9", "5120 × 1440 · 32:9",
            "1920 × 1200 · 16:10", "2560 × 1600 · 16:10",
            "1280 × 800 · 16:10", "1440 × 900 · 16:10",
            "1360 × 768 · 16:9",
            "1280 × 960 · 4:3", "1440 × 1080 · 4:3 (beta)", "1920 × 1440 · 4:3",
            "2048 × 1536 · 4:3 (beta)", "2560 × 1920 · 4:3 (beta)", "5120 × 2160 · 21:9",
            "8K · 7680 × 4320 · 16:9 (beta)"};
        static_assert(std::size(builtin_labels) == G::Resolutions.size());
        const std::vector<std::string> resolution_names = G::ResolutionNames();
        std::vector<const char*> resolution_labels;
        for (std::size_t i = 0; i < resolution_names.size(); ++i) {
            resolution_labels.push_back(i < G::Resolutions.size() ? builtin_labels[i] : resolution_names[i].c_str());
        }
        ChoiceRow("Render resolution", s.render_resolution, resolution_labels.data(), nullptr,
                  int(resolution_labels.size()), "The resolution the game draws at. Higher is sharper and slower.");
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        ImGui::EndTable();
    }
}

// The controller and the mouse (the launcher's Controls page sets the same, and the keys).
void ControlsTab() {
    auto& s = MtSettings::Get();
    Separator("Controller");
    if (BeginRows("##controller")) {
        SliderRow("Vibration", s.vibration, 0, 100, "%d%%",
                  "Strength of the controller's vibration. 0% turns it off.");
        ImGui::EndTable();
    }
    Separator("Mouse");
    if (BeginRows("##mouse")) {
        SliderRow("Camera speed", s.mouse_aim_sensitivity, 10, 200, "%d%%",
                  "How far the camera turns for the same mouse movement.");
        static const char* const styles[] = {"Relative", "Absolute"};
        ChoiceRow("Camera style", s.mouse_aim_style, styles, nullptr, 2,
                  "Relative: the camera turns as fast as the mouse moves. Absolute: it turns by how far the "
                  "mouse has gone since it started moving, so a long slow movement turns fast too.");
        CheckRow("Keep mouse in window", s.mouse_lock,
                 "While you play, the mouse stays in the game window; this menu and other windows get it back.");
        ImGui::EndTable();
    }
    Separator("Keys");
    ImGui::PushTextWrapPos(0.0f);
    DisabledText("The keyboard keys are set on the launcher's Controls page (MinusTwo.exe). The mouse "
                 "always moves the camera too.");
    ImGui::PopTextWrapPos();
}

// The cheats (src/runtime_cheats.c) and achievements on or off, applied at once; the launcher's
// Game and Achievements pages set the same.
void GameTab() {
    auto& s = MtSettings::Get();
    const int found = s.cheats_found;
    constexpr int all = MTGPU_CHEAT_NO_DAMAGE | MTGPU_CHEAT_INFINITE_BREATH | MTGPU_CHEAT_ALL_KAIJU;
    Separator("Cheats");
    if (found != all) {
        ImGui::PushTextWrapPos(0.0f);
        DisabledText(found ? "Some are unavailable: game version or code signature mismatch."
                           : "Unavailable: game version or code signature mismatch.");
        ImGui::PopTextWrapPos();
    }
    const auto cheat = [found](int bit, const char* label, std::atomic<bool>& value, const char* help) {
        ImGui::BeginDisabled(!(found & bit));
        CheckRow(label, value, help);
        ImGui::EndDisabled();
    };
    if (BeginRows("##cheats")) {
        cheat(MTGPU_CHEAT_NO_DAMAGE, "No damage", s.cheat_no_damage,
              "Godzilla's health is full again the moment he is hit.");
        cheat(MTGPU_CHEAT_INFINITE_BREATH, "Infinite atomic breath", s.cheat_infinite_breath,
              "The temperature is at its maximum again right after each atomic breath.");
        cheat(MTGPU_CHEAT_ALL_KAIJU, "Unlock all kaiju", s.cheat_all_kaiju,
              "Every kaiju is unlocked to play. The game saves them as unlocked: they stay unlocked "
              "when this is turned off.");
        ImGui::EndTable();
    }
    Separator("Achievements");
    if (BeginRows("##achievements")) {
        CheckRow("Unlock achievements", s.achievements,
                 "Off: the game unlocks no trophies and shows no notices. Those already unlocked stay.");
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = MtPerfStats;
    auto& s = MtSettings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    Separator("What to show");
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (game / total)", "VRAM (used / available)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(Tr(names[item]), &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        DisabledText("Preview");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        UiText(text.empty() ? "(nothing selected)" : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    Separator("Appearance");
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.");
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.");
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    Separator("About");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), Tr("loading %u%%"), u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "%s", Tr("ready"));
        }
        InfoRow("Shader cache", text);
        ImGui::EndTable();
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = MtSettings::Get();
    return (s.render_resolution != s.startup_render_resolution) +
           (s.monitor != s.startup_monitor) + (s.hdr != s.startup_hdr) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback) +
           (s.anisotropy != s.startup_anisotropy);
}

void Menu() {
    auto& s = MtSettings::Get();
    const float u = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(780.0f * u, viewport->WorkSize.x * 0.95f),
                                    std::min(720.0f * u, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // minustwo: the game's own title (param.sfo), not a fixed one.
    static const std::string title = [] {
        const std::string_view name = Common::ElfInfo::Instance().Title();
        return Upper(std::string(name.empty() ? "MinusTwo" : name).c_str());
    }();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f * u, 18.0f * u));
    ImGui::SetNextWindowBgAlpha(1.0f); // the game's text would show through (the blend is in linear light)
    const bool shown = ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!shown) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            s.menu_tab = (std::clamp(s.menu_tab.load(), 0, kMenuTabs - 1) + step + kMenuTabs) % kMenuTabs;
            dirty = true;
        }
    }
    const int current = std::clamp(s.menu_tab.load(), 0, kMenuTabs - 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const float left = ImGui::GetCursorScreenPos().x, right = left + ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(window, ImVec2(window.x + size.x, window.y + 3.0f * u), Theme::U32(Theme::Accent),
                      ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);

    // The brand, as the launcher's sidebar: ゴジラ in red over the title; the port and the keys beside.
    if (kana_font) {
        ImGui::PushFont(kana_font, 15.0f);
        ImGui::TextColored(Theme::Red, "%s", "\xe3\x82\xb4\xe3\x82\xb8\xe3\x83\xa9"); // ゴジラ
        ImGui::PopFont();
    }
    PushHeading(40.0f);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    const float baseline = ImGui::GetItemRectMax().y - ImGui::GetTextLineHeight() - 5.0f * u;
    ImGui::SameLine(0.0f, 14.0f * u);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, baseline));
    DisabledText("Minus Two · port menu");
#ifdef __APPLE__
    const char* keys = PadText("Cmd+, / R3+L2", "Cmd+, / RS+LT", "Cmd+, / RS+ZL");
#else
    const char* keys = PadText("Insert / R3+L2", "Insert / RS+LT", "Insert / RS+ZL");
#endif
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(right - ImGui::CalcTextSize(keys).x, baseline));
    ImGui::TextDisabled("%s", keys);

    // The tabs: names in capitals, the chosen one atomic blue and underlined, over a rule.
    static const char* const tabs[kMenuTabs] = {"Image", "Performance", "Controls", "Overlay", "Game"};
    ImGui::Dummy(ImVec2(0.0f, 10.0f * u));
    const ImVec2 strip = ImGui::GetCursorScreenPos();
    ImGui::PushFont(MtUi::chinese ? nullptr : nav_font, 19.0f);
    const float tab_h = ImGui::GetTextLineHeight();
    float x = strip.x;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true); // the pad and the keys move between the settings
    for (int tab = 0; tab < kMenuTabs; ++tab) {
        const std::string label = Upper(Tr(tabs[tab]));
        const float w = ImGui::CalcTextSize(label.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(x, strip.y));
        ImGui::PushID(tab);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, tab_h + 10.0f * u)) && tab != current) {
            s.menu_tab = tab;
            dirty = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool chosen = tab == current;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, strip.y),
                    Theme::U32(chosen ? Theme::AccentHi : hovered ? Theme::Text : Theme::Muted), label.c_str());
        if (chosen) {
            dl->AddRectFilled(ImVec2(x, strip.y + tab_h + 6.0f * u), ImVec2(x + w, strip.y + tab_h + 9.0f * u),
                              Theme::U32(Theme::AccentHi), 1.0f * u);
        }
        x += w + 30.0f * u;
    }
    ImGui::PopItemFlag();
    ImGui::PopFont();
    dl->AddLine(ImVec2(left, strip.y + tab_h + 9.0f * u), ImVec2(right, strip.y + tab_h + 9.0f * u),
                Theme::U32(Theme::Line), 1.0f * u);
    ImGui::SetCursorScreenPos(ImVec2(left, strip.y + tab_h + 16.0f * u));

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    ImGui::PushID(current); // each tab keeps its own scroll position
    if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
        switch (current) {
        case 0: ImageTab(); break;
        case 1: PerformanceTab(); break;
        case 2: ControlsTab(); break;
        case 3: OverlayTab(); break;
        default: GameTab(); break;
        }
    }
    ImGui::EndChild();
    ImGui::PopID();

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    DisabledText(footer_help ? footer_help : PadText("L1 / R1 or Q / E: switch tabs. Settings are saved in minustwo.ini.", "LB / RB or Q / E: switch tabs. Settings are saved in minustwo.ini.", "L / R or Q / E: switch tabs. Settings are saved in minustwo.ini."));
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
        ImGui::TextColored(Theme::Warn, Tr("%d change%s need%s a restart (unsaved progress is lost)."),
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::AccentHi);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Accent);
        ImGui::BeginDisabled(!std::getenv("MT_RESTART_COMMAND"));
        if (Button("Apply and restart")) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            MtSettings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::NewLine();
    }
    // Quit to desktop asks once more, then ends the game as the window's close button does (the
    // window loop ends the process; what the game has not saved yet is lost).
    static bool confirm_quit = false;
    if (confirm_quit) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Warn, "%s", Tr("Quit to desktop? Unsaved progress is lost."));
        ImGui::SameLine();
        if (Button("Quit")) {
            MtSettings::Save();
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        ImGui::SameLine();
        if (Button("Cancel")) {
            confirm_quit = false;
        }
    } else if (Button("Quit to desktop")) {
        confirm_quit = true;
    }
    ImGui::SameLine();
    const float close_width = ImGui::CalcTextSize(Tr("Close")).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        confirm_quit = false;
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(Theme::AccentHi, "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    DisabledText("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    DisabledText(PadText("Controller: Cross (A) = OK, Circle (B) = cancel", "Controller: A = OK, B = cancel", "Controller: B = OK, A = cancel"));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushTextWrapPos(viewport->WorkSize.x * 0.7f);
    const size_t details = choice_preview_label.empty() ? std::string::npos : choice_title.find("\n\n");
    ImGui::TextColored(Theme::AccentHi, "%.*s",
                       int(details == std::string::npos ? choice_title.size() : details), choice_title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (!choice_preview_label.empty()) {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(choice_preview_label.c_str());
        const ImVec2 preview_size(228.0f * base_scale, 128.0f * base_scale);
        ImTextureData* preview = choice_textures.size() > choice_items.size() ? choice_textures.back() : nullptr;
        if (preview) ImGui::Image(preview->GetTexRef(), preview_size);
        else ImGui::Dummy(preview_size);
        ImGui::EndGroup();
        if (details != std::string::npos) {
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(viewport->WorkSize.x * 0.7f);
            ImGui::TextColored(Theme::Soft, "%s", choice_title.c_str() + details + 2);
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
        ImGui::Separator();
    }
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(),
                                      choice_textures.begin() + std::min(choice_textures.size(), choice_items.size()),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(viewport->WorkSize.x * 0.7f);
        if (selected) {
            ImGui::TextColored(Theme::AccentHi, "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    Theme::U32(Theme::AccentHi), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    DisabledText(PadText("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel", "Up/Down or D-pad: choose   Enter or A: OK   Esc or B: cancel", "Up/Down or D-pad: choose   Enter or B: OK   Esc or A: cancel"));
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    UiText(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    DisabledText("The first start after an update takes longer.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = MtSettings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    UiText(text.c_str());
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled(Tr("Loading shader cache %u%%"), u32(u64(done) * 100 / total));
    ImGui::End();
}

// Trophy texts and list (set once by mtgpu_trophy_setup from the runtime, which reads them from the
// user's trophy_texts.txt, or GODZILLA's list built in): the labels of the notice and of the in-game
// trophy window.
struct TrophyLabels {
    std::string heading = "Trophies", notice = "TROPHY UNLOCKED", unlocked = "UNLOCKED", locked = "LOCKED";
    std::string all = "All trophies", only_unlocked = "Unlocked", only_locked = "Locked";
    std::string counter = "{} of {} unlocked";
    std::string hint = "Up/Down: scroll   Left/Right: filter   F8 / R3+R2 / Esc: close";
};
struct TrophyRow {
    int id = 0;
    std::string name, detail;
    MtPng::Image image;         // decoded icon, waiting for its texture
    ImTextureData* tex = nullptr; // render thread, lives for the whole run
};
TrophyLabels trophy_labels;                      // imgui_mutex
std::vector<TrophyRow> trophy_rows;              // imgui_mutex
const volatile uint32_t* trophy_flags = nullptr; // the runtime's unlocked bits, read live
std::atomic<bool> trophy_ready{false}, trophy_started{false};
std::atomic<bool> trophy_view{false};            // the in-game trophy window is open
int trophy_filter = 0;                           // 0 all, 1 unlocked, 2 locked
float trophy_scroll = 0.0f;                      // pixels

// Trophy notices: queued by mtgpu_toast/mtgpu_toast_icon (any thread), drawn by Render in the
// top-right corner, below the FPS counter.
struct Toast {
    std::string text, detail;     // title (the trophy's name) and the line under it (its description)
    std::chrono::steady_clock::time_point start, until;
    MtPng::Image image;         // decoded icon, waiting for its texture (empty: none)
    ImTextureData* tex = nullptr; // render thread only
};
std::mutex toast_mutex;
std::vector<Toast> toasts;
std::vector<ImTextureData*> toast_retired; // render thread only; freed once the backend destroyed them
std::atomic<bool> toast_active{false};
constexpr float kToastSeconds = 5.0f;

// imgui_mutex held, before NewFrame: icons that arrived become textures, destroyed ones are freed.
void UpdateToastTextures() {
    std::scoped_lock lock{toast_mutex};
    for (Toast& t : toasts) {
        if (t.tex || t.image.rgba.empty()) {
            continue;
        }
        ImTextureData* tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, t.image.width, t.image.height);
        std::memcpy(tex->GetPixels(), t.image.rgba.data(), t.image.rgba.size());
        ImGui::RegisterUserTexture(tex);
        t.tex = tex;
        t.image = {};
    }
    std::erase_if(toast_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    toast_active = !toasts.empty() || !toast_retired.empty();
}

// A gold medal with a cup, drawn with shapes: used when a trophy has no icon. a: overall opacity.
void DrawTrophyIcon(ImDrawList* dl, ImVec2 c, float r, float a) {
    auto col = [&](float R, float G, float B, float A) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(R, G, B, A * a));
    };
    dl->AddCircleFilled(c, r, col(0.96f, 0.74f, 0.20f, 1.0f), 48);
    dl->AddCircle(c, r, col(1.0f, 0.92f, 0.60f, 1.0f), 48, std::max(1.5f, r * 0.07f));
    const ImU32 ink = col(0.12f, 0.08f, 0.02f, 1.0f);
    const float u = r / 10.0f;
    dl->AddCircle(ImVec2(c.x - 5.4f * u, c.y - 2.6f * u), 2.2f * u, ink, 20, 1.3f * u);
    dl->AddCircle(ImVec2(c.x + 5.4f * u, c.y - 2.6f * u), 2.2f * u, ink, 20, 1.3f * u);
    dl->AddQuadFilled(ImVec2(c.x - 4.6f * u, c.y - 5.8f * u), ImVec2(c.x + 4.6f * u, c.y - 5.8f * u),
                      ImVec2(c.x + 2.4f * u, c.y + 1.6f * u), ImVec2(c.x - 2.4f * u, c.y + 1.6f * u), ink);
    dl->AddRectFilled(ImVec2(c.x - 0.9f * u, c.y + 1.6f * u), ImVec2(c.x + 0.9f * u, c.y + 4.4f * u), ink);
    dl->AddRectFilled(ImVec2(c.x - 3.2f * u, c.y + 4.4f * u), ImVec2(c.x + 3.2f * u, c.y + 6.0f * u), ink, 0.5f * u);
}

void ToastBox() {
    const auto now = std::chrono::steady_clock::now();
    struct Line {
        std::string text, detail;
        float elapsed, left;
        ImTextureData* tex;
    };
    std::vector<Line> lines;
    {
        std::scoped_lock lock{toast_mutex};
        for (Toast& t : toasts) {
            if (t.until <= now && t.tex) {
                t.tex->WantDestroyNextFrame = true;
                toast_retired.push_back(t.tex);
                t.tex = nullptr;
            }
        }
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                                    [&](const Toast& t) { return t.until <= now; }),
                     toasts.end());
        for (const Toast& t : toasts) {
            lines.push_back({t.text, t.detail, std::chrono::duration<float>(now - t.start).count(),
                             std::chrono::duration<float>(t.until - now).count(), t.tex});
        }
        toast_active = !toasts.empty() || !toast_retired.empty();
    }
    if (lines.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    const float pad = 20.0f * s, w = 380.0f * s, gap = 10.0f * s;
    const float text_x = 96.0f * s, wrap_w = w - text_x - 16.0f * s;
    const float fs = ImGui::GetFontSize();
    constexpr float kNameScale = 1.3f;
    // Start below the FPS counter when it is showing in the top-right half of the screen.
    float y = viewport->WorkPos.y + pad;
    if (ImGuiWindow* fps = ImGui::FindWindowByName("##fps"); fps && fps->Active) {
        const float mid_x = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
        const float mid_y = viewport->WorkPos.y + viewport->WorkSize.y * 0.5f;
        if (fps->Pos.y < mid_y && fps->Pos.x + fps->Size.x > mid_x) {
            y = std::max(y, fps->Pos.y + fps->Size.y + gap);
        }
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const Line& l = lines[i];
        // The card grows with the wrapped name and description (never under 88 px).
        const float name_h = ImGui::CalcTextSize(l.text.c_str(), nullptr, false, wrap_w / kNameScale).y * kNameScale;
        const float detail_h = l.detail.empty() ? 0.0f : ImGui::CalcTextSize(l.detail.c_str(), nullptr, false, wrap_w).y;
        const float block = fs * 1.15f + name_h + (detail_h > 0.0f ? 4.0f * s + detail_h : 0.0f);
        const float h = std::max(88.0f * s, block + 28.0f * s);
        const float tin = std::min(1.0f, l.elapsed / 0.4f);
        const float ease = 1.0f - (1.0f - tin) * (1.0f - tin) * (1.0f - tin); // slides in from the right
        const float alpha = std::clamp(std::min(l.left / 0.5f, l.elapsed / 0.2f), 0.0f, 1.0f);
        const float x = viewport->WorkPos.x + viewport->WorkSize.x - pad + (1.0f - ease) * (w + pad);
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Alpha(Theme::Panel, 0.94f * alpha));
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Alpha(Theme::AccentHi, 0.9f * alpha));
        char name[40];
        std::snprintf(name, sizeof(name), "##trophy_toast_%zu", i);
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        const ImU32 accent = Theme::U32(Theme::AccentHi, alpha);
        const ImU32 accent_soft = Theme::U32(Theme::AccentHi, 0.75f * alpha);
        dl->AddRectFilled(p, ImVec2(p.x + 6.0f * s, p.y + h), accent, 10.0f * s, ImDrawFlags_RoundCornersLeft);
        if (l.tex) {
            const float icon = 64.0f * s;
            ImGui::SetCursorPos(ImVec2(18.0f * s, (h - 8.0f * s - icon) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::Image(l.tex->GetTexRef(), ImVec2(icon, icon));
            ImGui::PopStyleVar();
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), accent, 3.0f * s, 0, 2.0f * s);
        } else {
            DrawTrophyIcon(dl, ImVec2(p.x + 52.0f * s, p.y + (h - 8.0f * s) * 0.5f), 28.0f * s, alpha);
        }
        const float frac = std::clamp(l.left / kToastSeconds, 0.0f, 1.0f); // time left
        dl->AddRectFilled(ImVec2(p.x + 14.0f * s, p.y + h - 8.0f * s),
                          ImVec2(p.x + 14.0f * s + (w - 28.0f * s) * frac, p.y + h - 5.0f * s), accent_soft, 1.5f * s);
        const float top = std::max(8.0f * s, (h - 8.0f * s - block) * 0.5f);
        ImGui::PushTextWrapPos(w - 16.0f * s);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SetCursorPos(ImVec2(text_x, top));
        ImGui::TextColored(Theme::Alpha(Theme::AccentHi, alpha), "%s", trophy_labels.notice.c_str());
        ImGui::SetCursorPos(ImVec2(text_x, top + fs * 1.15f));
        ImGui::SetWindowFontScale(kNameScale);
        ImGui::TextColored(Theme::Alpha(Theme::Text, alpha), "%s", l.text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        if (detail_h > 0.0f) {
            ImGui::SetCursorPos(ImVec2(text_x, top + fs * 1.15f + name_h + 4.0f * s));
            ImGui::TextColored(Theme::Alpha(Theme::Soft, alpha), "%s", l.detail.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        y += h + gap;
    }
}

bool TrophyUnlocked(int id) {
    return trophy_flags && id >= 0 && id < 128 && ((trophy_flags[id / 32] >> (id % 32)) & 1u);
}

void SetTrophyView(bool open) {
    trophy_view = open;
    trophy_scroll = 0.0f;
    trophy_filter = 0;
}

// imgui_mutex held, before NewFrame: the icons of the list become textures (once).
void UpdateTrophyTextures() {
    for (TrophyRow& row : trophy_rows) {
        if (row.tex || row.image.rgba.empty()) {
            continue;
        }
        ImTextureData* tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, row.image.width, row.image.height);
        std::memcpy(tex->GetPixels(), row.image.rgba.data(), row.image.rgba.size());
        ImGui::RegisterUserTexture(tex);
        row.tex = tex;
        row.image = {};
    }
}

constexpr float kTrophyRow = 80.0f, kTrophyGap = 6.0f; // layout units (x base_scale)

// The list takes the keys and buttons that scroll, filter and close; the rest of the input stays
// with the game (held neutral by CapturesInput).
bool HandleTrophyEvent(const SDL_Event& event) {
    const float step = (kTrophyRow + kTrophyGap) * base_scale;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode key = event.key.key;
        if (key == SDLK_UP || key == SDLK_W) {
            trophy_scroll -= step;
        } else if (key == SDLK_DOWN || key == SDLK_S) {
            trophy_scroll += step;
        } else if (key == SDLK_PAGEUP) {
            trophy_scroll -= 4.0f * step;
        } else if (key == SDLK_PAGEDOWN) {
            trophy_scroll += 4.0f * step;
        } else if (key == SDLK_HOME) {
            trophy_scroll = 0.0f;
        } else if (key == SDLK_END) {
            trophy_scroll = 1.0e6f; // clamped by the window
        } else if (key == SDLK_LEFT || key == SDLK_A || key == SDLK_RIGHT || key == SDLK_D) {
            trophy_filter = (trophy_filter + (key == SDLK_LEFT || key == SDLK_A ? 2 : 1)) % 3;
            trophy_scroll = 0.0f;
        } else if (!event.key.repeat && (key == SDLK_F8 || key == SDLK_ESCAPE || key == SDLK_BACKSPACE ||
                                         key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_INSERT)) {
            SetTrophyView(false);
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        trophy_scroll -= event.wheel.y * step;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
            if (down && r2_down) {
                SetTrophyView(false);
            }
        } else if (down) {
            if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                trophy_scroll -= step;
            } else if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                trophy_scroll += step;
            } else if (button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ||
                       button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                const bool back = button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
                trophy_filter = (trophy_filter + (back ? 2 : 1)) % 3;
                trophy_scroll = 0.0f;
            } else if (button == SDL_GAMEPAD_BUTTON_EAST || button == SDL_GAMEPAD_BUTTON_SOUTH) {
                SetTrophyView(false);
            }
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !r2_down && r3_down) {
                r2_down = true;
                SetTrophyView(false);
                return true;
            }
            r2_down = down;
        } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            l2_down = event.gaxis.value > 16000;
        }
        return false;
    default:
        return false;
    }
}

std::string TrophyCounter(int have, int total) {
    std::string text = trophy_labels.counter;
    for (const int value : {have, total}) {
        const size_t at = text.find("{}");
        if (at == std::string::npos) {
            break;
        }
        text.replace(at, 2, std::to_string(value));
    }
    return text;
}

// The trophy list, centered over the game: icon, name, description and state of every trophy.
void TrophyView() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    const ImVec2 size(std::min(780.0f * s, viewport->WorkSize.x - 40.0f * s),
                      std::min(660.0f * s, viewport->WorkSize.y - 60.0f * s));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Alpha(Theme::Panel, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Line);
    ImGui::Begin("##trophy_list", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    ImGui::SetWindowFontScale(1.0f);
    const float fs = ImGui::GetFontSize();
    const ImVec4 accent = Theme::AccentHi, white = Theme::Text, muted = Theme::Muted;
    auto color = [](const ImVec4& c, float a = 1.0f) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a));
    };

    int have = 0;
    std::vector<const TrophyRow*> shown;
    for (const TrophyRow& row : trophy_rows) {
        const bool got = TrophyUnlocked(row.id);
        have += got ? 1 : 0;
        if (trophy_filter == 0 || (trophy_filter == 1 && got) || (trophy_filter == 2 && !got)) {
            shown.push_back(&row);
        }
    }

    // header: heading, counter and the filter
    const float title_h = fs * 1.5f;
    const float header_h = 16.0f * s + title_h + 10.0f * s + fs + 16.0f * s;
    const float footer_h = fs + 22.0f * s;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + header_h), color(Theme::Bg), 12.0f * s,
                      ImDrawFlags_RoundCornersTop);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::SetCursorPos(ImVec2(24.0f * s, 16.0f * s));
    ImGui::TextColored(accent, "%s", trophy_labels.heading.c_str());
    ImGui::SetWindowFontScale(1.0f);
    const std::string counter = TrophyCounter(have, int(trophy_rows.size()));
    ImGui::SetCursorPos(ImVec2(size.x - 24.0f * s - ImGui::CalcTextSize(counter.c_str()).x, 16.0f * s + (title_h - fs) * 0.5f));
    ImGui::TextColored(white, "%s", counter.c_str());
    const std::string* filters[3] = {&trophy_labels.all, &trophy_labels.only_unlocked, &trophy_labels.only_locked};
    float fx = 24.0f * s;
    const float fy = 16.0f * s + title_h + 10.0f * s;
    for (int i = 0; i < 3; i++) {
        const float w = ImGui::CalcTextSize(filters[i]->c_str()).x;
        ImGui::SetCursorPos(ImVec2(fx, fy));
        ImGui::TextColored(i == trophy_filter ? accent : muted, "%s", filters[i]->c_str());
        if (i == trophy_filter) {
            dl->AddRectFilled(ImVec2(p.x + fx, p.y + fy + fs + 3.0f * s), ImVec2(p.x + fx + w, p.y + fy + fs + 5.0f * s),
                              color(accent), 1.0f * s);
        }
        fx += w + 28.0f * s;
    }

    // list, drawn by hand so it scrolls by pixels
    const ImVec2 list_min(p.x, p.y + header_h), list_max(p.x + size.x, p.y + size.y - footer_h);
    const float step = (kTrophyRow + kTrophyGap) * s;
    const float content_h = float(shown.size()) * step + 12.0f * s;
    const float max_scroll = std::max(0.0f, content_h - (list_max.y - list_min.y));
    trophy_scroll = std::clamp(trophy_scroll, 0.0f, max_scroll);
    const float status_w = std::max(ImGui::CalcTextSize(trophy_labels.unlocked.c_str()).x,
                                    ImGui::CalcTextSize(trophy_labels.locked.c_str()).x);
    ImGui::PushClipRect(list_min, list_max, true);
    for (size_t k = 0; k < shown.size(); k++) {
        const TrophyRow& row = *shown[k];
        const float row_h = kTrophyRow * s;
        const float y = list_min.y + 8.0f * s + float(k) * step - trophy_scroll;
        if (y + row_h < list_min.y || y > list_max.y) {
            continue;
        }
        const bool got = TrophyUnlocked(row.id);
        const ImVec2 a(p.x + 16.0f * s, y), b(p.x + size.x - 16.0f * s - (max_scroll > 0.0f ? 8.0f * s : 0.0f), y + row_h);
        dl->AddRectFilled(a, b, color(got ? Theme::Card : Theme::Rgb(13, 18, 26)), 8.0f * s);
        dl->AddRect(a, b, color(got ? Theme::Alpha(Theme::Accent, 0.9f) : Theme::Line), 8.0f * s, 0,
                    1.5f * s);
        ImGui::PushClipRect(a, b, true);
        const float icon = 56.0f * s;
        const ImVec2 ip(a.x + 12.0f * s, a.y + (row_h - icon) * 0.5f);
        ImGui::SetCursorScreenPos(ip);
        if (row.tex) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, got ? 1.0f : 0.35f);
            ImGui::Image(row.tex->GetTexRef(), ImVec2(icon, icon));
            ImGui::PopStyleVar();
        } else {
            ImGui::Dummy(ImVec2(icon, icon));
        }
        dl->AddRect(ip, ImVec2(ip.x + icon, ip.y + icon), color(got ? accent : Theme::Line), 3.0f * s, 0,
                    1.5f * s);
        const float tx = ip.x + icon + 14.0f * s;
        const float status_x = b.x - 16.0f * s - status_w;
        ImGui::SetWindowFontScale(1.15f);
        ImGui::SetCursorScreenPos(ImVec2(tx, a.y + 9.0f * s));
        ImGui::PushTextWrapPos(status_x - 12.0f * s - p.x);
        ImGui::TextColored(got ? white : muted, "%s", row.name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(1.0f);
        if (!row.detail.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(tx, a.y + 9.0f * s + fs * 1.15f + 4.0f * s));
            ImGui::PushTextWrapPos(status_x - 12.0f * s - p.x);
            ImGui::TextColored(got ? Theme::Soft : Theme::Alpha(Theme::Muted, 0.8f), "%s",
                               row.detail.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorScreenPos(ImVec2(status_x, a.y + (row_h - fs) * 0.5f));
        ImGui::TextColored(got ? accent : muted, "%s", (got ? trophy_labels.unlocked : trophy_labels.locked).c_str());
        ImGui::PopClipRect();
    }
    ImGui::PopClipRect();
    if (max_scroll > 0.0f) { // scroll bar
        const float track = list_max.y - list_min.y - 16.0f * s;
        const float thumb = std::max(24.0f * s, track * (list_max.y - list_min.y) / content_h);
        const float top = list_min.y + 8.0f * s + (track - thumb) * (trophy_scroll / max_scroll);
        dl->AddRectFilled(ImVec2(p.x + size.x - 14.0f * s, top), ImVec2(p.x + size.x - 8.0f * s, top + thumb),
                          color(accent, 0.7f), 3.0f * s);
    }
    // footer: the keys
    const float hint_w = ImGui::CalcTextSize(trophy_labels.hint.c_str()).x;
    ImGui::SetCursorPos(ImVec2(std::max(12.0f * s, (size.x - hint_w) * 0.5f), size.y - footer_h + 10.0f * s));
    ImGui::TextColored(muted, "%s", trophy_labels.hint.c_str());
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

// Lower-right notes stack upward from `bottom`: the screenshot notice, then the shader notes.
template <typename Body>
void CornerNote(const char* id, float& bottom, float alpha, Body&& body) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, bottom),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    body();
    bottom -= ImGui::GetWindowHeight() + pad / 2;
    ImGui::End();
}

void Notice(float& bottom) {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    CornerNote("##notice", bottom, 0.6f, [&] { UiText(text.c_str()); });
}

// Launcher: "Shader notes in the corner" (MT_SHADER_NOTES), off by default.
const bool shader_notes = [] {
    const char* env = std::getenv("MT_SHADER_NOTES");
    return env && *env != '0';
}();

bool PrefetchActive() {
    const u32 total = prefetch_total;
    return shader_notes && total && (prefetch_done < total || prefetch_until > NowMs());
}

void ShaderPrefetch(float& bottom) {
    const u32 done = prefetch_done, total = std::max(prefetch_total.load(), 1u);
    CornerNote("##shader_prefetch", bottom, 0.35f, [&] {
        if (done >= total) {
            ImGui::TextDisabled("Shader cache ready (%u)", total);
            return;
        }
        ImGui::TextDisabled("Reading shader cache %u%% (%u/%u)", u32(u64(done) * 100 / total), done,
                            total);
        ImGui::ProgressBar(float(done) / float(total),
                           ImVec2(ImGui::GetContentRegionAvail().x, 4.0f * base_scale), "");
    });
}

bool CompileNoteActive() {
    return shader_notes && (compile_pending || compile_until > NowMs());
}

void ShaderCompiles(float& bottom) {
    const u32 pending = compile_pending, count = compile_count;
    CornerNote("##shader_compiles", bottom, 0.35f, [&] {
        if (pending) {
            ImGui::TextDisabled("Compiling shaders (%u left)", pending);
        } else {
            ImGui::TextDisabled("Compiled %u new shader%s", count, count == 1 ? "" : "s");
        }
    });
}

} // namespace

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void CycleEngineFps() {
    const int fps = MtSettings::CycleEngineFps();
    ShowNotice(fps ? std::string(Tr("Engine frame rate: ")) + std::to_string(fps) + Tr(" FPS")
                   : std::string(Tr("Engine frame rate: 60 FPS only (no frame rate patch for this game build)")));
}

// The launcher's colours on every window of the overlay (menu, save list, notices).
void ApplyTheme(ImGuiStyle& style) {
    using namespace Theme;
    ImVec4* c = style.Colors;
    constexpr ImVec4 none(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Text] = Text;
    c[ImGuiCol_TextDisabled] = Muted;
    c[ImGuiCol_WindowBg] = Alpha(Panel, 0.95f);
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = Alpha(Card, 0.98f);
    c[ImGuiCol_Border] = Line;
    c[ImGuiCol_FrameBg] = Card;
    c[ImGuiCol_FrameBgHovered] = Raised;
    c[ImGuiCol_FrameBgActive] = Rgb(31, 46, 64);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = Bg;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = Line;
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(42, 58, 78);
    c[ImGuiCol_ScrollbarGrabActive] = Accent;
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // a white tick on atomic blue, as the launcher's
    c[ImGuiCol_CheckboxSelectedBg] = Accent;
    c[ImGuiCol_SliderGrab] = Accent;
    c[ImGuiCol_SliderGrabActive] = AccentHi;
    c[ImGuiCol_Button] = Card;
    c[ImGuiCol_ButtonHovered] = Raised;
    c[ImGuiCol_ButtonActive] = Accent;
    c[ImGuiCol_Header] = Alpha(Accent, 0.35f);
    c[ImGuiCol_HeaderHovered] = Alpha(Accent, 0.55f);
    c[ImGuiCol_HeaderActive] = Accent;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = Line;
    c[ImGuiCol_SeparatorActive] = Accent;
    c[ImGuiCol_ResizeGrip] = none;
    c[ImGuiCol_ResizeGripHovered] = Alpha(Accent, 0.5f);
    c[ImGuiCol_ResizeGripActive] = Accent;
    c[ImGuiCol_PlotHistogram] = Accent;
    c[ImGuiCol_PlotHistogramHovered] = AccentHi;
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = Line;
    c[ImGuiCol_TextSelectedBg] = Alpha(Accent, 0.4f);
    c[ImGuiCol_NavCursor] = AccentHi;
}

// A font from the system's fonts folder (Windows), or null.
ImFont* SystemFont(ImFontAtlas* atlas, const char* file) {
#ifdef _WIN32
    wchar_t directory[MAX_PATH]{};
    const UINT size = GetWindowsDirectoryW(directory, MAX_PATH);
    if (!size || size >= MAX_PATH) {
        return nullptr;
    }
    const auto path = std::filesystem::path(directory) / "Fonts" / file;
    if (!MtUi::UsableFont(path)) {
        return nullptr;
    }
    const auto utf8 = path.u8string();
    return atlas->AddFontFromFileTTF(reinterpret_cast<const char*>(utf8.c_str()), 18.0f);
#else
    (void)atlas;
    (void)file;
    return nullptr;
#endif
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "minustwo";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    ApplyTheme(style);

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(mt_font_ttf),
                                   int(mt_font_ttf_end - mt_font_ttf), 18.0f, &font_config);
    const char* ui_language = std::getenv("MT_UI_LANGUAGE");
    if (!MtUi::ConfigureChineseFont(io.Fonts, ui_language ? ui_language : "", MtUi::FindChineseFont()) &&
        MtUi::WantsChinese(ui_language ? ui_language : "")) {
        std::printf("Overlay: no usable system CJK font; keeping English menu text\n");
    }
    heading_font = SystemFont(io.Fonts, "impact.ttf");
    nav_font = SystemFont(io.Fonts, "bahnschrift.ttf");
    kana_font = SystemFont(io.Fonts, "YuGothB.ttc");


    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const MtPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    if (trophy_view && !menu_open) {
        return HandleTrophyEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("MT_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (event.key.key == SDLK_F9) {
            if (down && !event.key.repeat) {
                CycleEngineFps();
            }
            return true;
        }
        if (event.key.key == SDLK_F12) {
            if (down && !event.key.repeat) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
            return true;
        }
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        if (down && !event.key.repeat && event.key.key == SDLK_F8 && !is_open && !text_entry_active && trophy_ready) {
            SetTrophyView(true); // the trophy list
            return true;
        }
#ifdef __APPLE__
        // minustwo (macOS): Mac keyboards have no Insert key; Command-comma opens settings there.
        const bool menu_key = event.key.key == SDLK_INSERT ||
                              (event.key.key == SDLK_COMMA && (event.key.mod & SDL_KMOD_GUI));
#else
        const bool menu_key = event.key.key == SDLK_INSERT;
#endif
        if (down && !event.key.repeat && (menu_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(menu_key ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // R3+L2 opens the menu: L3+R3 is a game action.
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
            if (down && l2_down) {
                SetOpen(!is_open);
                return true;
            }
            if (down && r2_down && !is_open && trophy_ready) { // R3+R2: the trophy list
                SetTrophyView(true);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !l2_down && r3_down) {
                l2_down = true;
                SetOpen(!is_open);
                return true;
            }
            l2_down = down;
        } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !r2_down && r3_down && !is_open && trophy_ready) {
                r2_down = true;
                SetTrophyView(true);
                return true;
            }
            r2_down = down;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = MtSettings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    MtSettings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetShaderPrefetch(u32 done, u32 total) {
    if (total && done >= total) {
        prefetch_until = NowMs() + 2000;
    }
    prefetch_done = done;
    prefetch_total = total;
}

void NoteShaderCompiles(u32 pending, u32 started) {
    const s64 now = NowMs();
    if (!compile_pending && compile_until <= now) {
        compile_count = 0; // a new burst
    }
    compile_count += started;
    compile_pending = pending;
    compile_until = now + 2000;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

// MT_MENU_AT=<seconds>: the menu opens that long after the first frame and shows the next tab
// every 4 seconds (pictures of the menu in test runs, with nothing pressed).
void OpenMenuForTest() {
    static const float menu_at = [] {
        const char* env = std::getenv("MT_MENU_AT");
        return env ? float(std::atof(env)) : 0.0f;
    }();
    if (menu_at <= 0.0f) {
        return;
    }
    static const auto first = std::chrono::steady_clock::now();
    static int shown_tab = -1;
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - first).count() - menu_at;
    if (t >= 0.0f && int(t / 4.0f) != shown_tab) {
        std::scoped_lock lock{imgui_mutex};
        if (initialized) {
            shown_tab = int(t / 4.0f);
            SetOpen(true);
            MtSettings::Get().menu_tab = shown_tab % kMenuTabs;
        }
    }
}

// Asked by the presenter every frame: the overlay draws only when this is true.
bool Visible() {
    OpenMenuForTest();
    const auto& s = MtSettings::Get();
    return initialized && (loading || background_total || PrefetchActive() || CompileNoteActive() ||
                           menu_open || text_entry_active || choice_active || choice_retiring ||
                           toast_active || trophy_view || s.show_fps || NoticeActive());
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active || trophy_view;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<MtPng::Image> pictures, int focus,
                 const std::string& preview_label, MtPng::Image preview) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_preview_label = preview_label;
    if (!preview_label.empty()) choice_pictures.push_back(std::move(preview));
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    UpdateToastTextures();
    UpdateTrophyTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
    }
    if (MtSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    float corner = ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y -
                   12.0f * base_scale;
    if (NoticeActive()) {
        Notice(corner);
    }
    if (PrefetchActive() && !loading && !menu_open) {
        ShaderPrefetch(corner);
    }
    if (CompileNoteActive() && !loading && !menu_open) {
        ShaderCompiles(corner);
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    if (trophy_view && !menu_open) {
        TrophyView();
    }
    if (toast_active) {
        ToastBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace MtOverlay

// Called by the runtime (src/runtime_services.c) when the game unlocks a trophy. title: its name,
// detail: its description (may be empty), png: the trophy's icon (the PNG bytes from trophy00.trp)
// or null for the drawn medal.
extern "C" void mtgpu_toast_trophy(const char* title, const char* detail, const unsigned char* png,
                                     uint64_t png_size) {
    if (!title || !title[0]) {
        return;
    }
    MtPng::Image image;
    if (png && png_size) {
        MtPng::DecodeRgba(png, size_t(png_size), image); // empty on failure: the medal is drawn
    }
    using namespace MtOverlay;
    std::scoped_lock lock{toast_mutex};
    const auto now = std::chrono::steady_clock::now();
    if (toasts.size() >= 6) {
        toasts.front().until = now; // expires now; Render retires its texture
    }
    toasts.push_back({title, detail ? detail : "", now,
                      now + std::chrono::milliseconds(int(kToastSeconds * 1000.0f)), std::move(image)});
    toast_active = true;
}

extern "C" void mtgpu_toast_icon(const char* text, const unsigned char* png, uint64_t png_size) {
    mtgpu_toast_trophy(text, nullptr, png, png_size);
}

extern "C" void mtgpu_toast(const char* text) {
    mtgpu_toast_trophy(text, nullptr, nullptr, 0);
}

// Called once by the runtime (src/runtime_services.c) with the trophy texts: the in-game list
// (F8 / R3+R2) and the notice labels. The icons are decoded on a worker thread; unlocked is read live.
extern "C" void mtgpu_trophy_setup(const MtGpuTrophyLabels* labels, const MtGpuTrophyInfo* items, int count,
                                     const volatile uint32_t* unlocked) {
    using namespace MtOverlay;
    if (!items || count <= 0 || trophy_started.exchange(true)) {
        return;
    }
    struct Item {
        int id;
        std::string name, detail;
        std::vector<unsigned char> png;
    };
    std::vector<Item> list;
    for (int i = 0; i < count; i++) {
        list.push_back({items[i].id, items[i].name ? items[i].name : "", items[i].detail ? items[i].detail : "",
                        items[i].png && items[i].png_size
                            ? std::vector<unsigned char>(items[i].png, items[i].png + items[i].png_size)
                            : std::vector<unsigned char>()});
    }
    TrophyLabels copy;
    if (labels) {
        auto take = [](std::string& to, const char* from) {
            if (from && from[0]) {
                to = from;
            }
        };
        take(copy.heading, labels->heading);
        take(copy.notice, labels->notice);
        take(copy.unlocked, labels->status_unlocked);
        take(copy.locked, labels->status_locked);
        take(copy.all, labels->filter_all);
        take(copy.only_unlocked, labels->filter_unlocked);
        take(copy.only_locked, labels->filter_locked);
        take(copy.counter, labels->counter);
        take(copy.hint, labels->hint);
    }
    std::thread([list = std::move(list), copy = std::move(copy), unlocked]() mutable {
        std::vector<TrophyRow> rows;
        for (Item& item : list) {
            TrophyRow row;
            row.id = item.id;
            row.name = std::move(item.name);
            row.detail = std::move(item.detail);
            if (!item.png.empty()) {
                MtPng::DecodeRgba(item.png.data(), item.png.size(), row.image); // empty on failure
            }
            rows.push_back(std::move(row));
        }
        std::scoped_lock lock{imgui_mutex};
        trophy_rows = std::move(rows);
        trophy_labels = std::move(copy);
        trophy_flags = unlocked;
        trophy_ready = true;
    }).detach();
}
