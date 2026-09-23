#include "debug_overlay.h"

#include "frame_limiter.h"
#include "game_access.h"
#include "live_geometry.h"
#include "../../src/renderer.h"
#include "../../src/world_mesh.h"
#include "log.h"
#include "overlay.h"
#include "remix_api.h"
#include "remix_lights.h"
#include "settings.h"
#include "synthetic_lights.h"
#include "texture_store.h"
#include "time_of_day.h"
#include "world_view.h"

#include <cmath>
#include <d3d9.h>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace gta2dx9 {
namespace {

bool    g_initialised = false;
bool    g_visible = false;
HWND    g_inputWindow = nullptr;     // the game's; owns the keyboard focus
HWND    g_presentWindow = nullptr;   // ours; what the panel is drawn into
WNDPROC g_previousWndProc = nullptr;
bool    g_f4Down = false;

// The one switch that decides how much of this menu exists. Off, it is the
// handful of controls a player wants; on, it grows the tables, the thresholds
// and the per-light editor the port was tuned with. Nothing was deleted to make
// the simple view simple - it is hidden behind here.
bool g_advanced = false;

uint64_t g_selected = 0;
bool     g_resetWindowPos = true;
bool     g_resetWindowSize = true;

// Applied scale, and the user's preference (0 = follow the display).
float g_scale = 1.0f;
float g_scaleSetting = 0.0f;

// Filters for the light table. A district with a hundred lamps in view is
// unusable without them.
char g_filter[64] = "";
bool g_showStatic = true;
bool g_showMoving = true;
bool g_showDark = true;
bool g_onlyOverridden = false;
bool g_onlyUndrawn = false;

const ImVec4 kGood = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kBad = ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
const ImVec4 kWarn = ImVec4(0.95f, 0.80f, 0.35f, 1.0f);
const ImVec4 kDim = ImVec4(0.62f, 0.62f, 0.62f, 1.0f);

inline bool Adv() { return g_advanced; }

// Every hardcoded pixel size in this file goes through here, so the panel scales
// as one piece rather than as scaled text inside unscaled tables.
inline float S(float v) { return v * g_scale; }

// The replacement for a paragraph of TextWrapped: the explanation is still
// here, it just costs a hover instead of a third of the panel.
void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("%s", text);
}

// The display's shape, as a ratio people recognise, so the stretch option can
// name what it stretches to rather than assume 16:9. Anything uncommon is left
// unnamed instead of guessed at - a 3440x1440 ultrawide is 2.39, not the 2.33
// its "21:9" badge claims, and a made-up number reads worse than none.
const char* DisplayAspect() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x <= 0.0f || size.y <= 0.0f) return "";
    const float ratio = size.x / size.y;
    struct Known { float ratio; const char* name; };
    static const Known known[] = {
        {4.0f / 3.0f, "4:3"},    {5.0f / 4.0f, "5:4"},   {16.0f / 10.0f, "16:10"},
        {16.0f / 9.0f, "16:9"},  {2.37f, "21:9"},        {32.0f / 9.0f, "32:9"},
    };
    for (const Known& k : known) {
        if (fabsf(ratio - k.ratio) < k.ratio * 0.03f) return k.name;
    }
    return "";
}

void Toggle() {
    g_visible = !g_visible;
    ImGuiIO& io = ImGui::GetIO();
    // The game hides the system cursor, so ImGui draws its own while the panel
    // is up; without this there is nothing on screen to aim with.
    io.MouseDrawCursor = g_visible;
    if (g_visible) {
        // Closing and reopening is the way back from a window dragged somewhere
        // useless, so the position is reset every time it comes up.
        g_resetWindowPos = true;
    }
}

LRESULT CALLBACK MenuWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_initialised && g_visible && ImGui::GetCurrentContext()) {
        const bool keyMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if (keyMessage || message == WM_MOUSEWHEEL) {
            // The panel is drawn into our own window, so the handler is told that
            // one; for key and wheel messages it ignores the hwnd anyway.
            ImGui_ImplWin32_WndProcHandler(g_presentWindow, message, wparam, lparam);
            // Only swallowed while a text field actually has the caret. GTA2
            // reads the keyboard through DirectInput as well, which no amount of
            // message filtering reaches, so this is a courtesy rather than a
            // guarantee - stand still while typing.
            if (ImGui::GetIO().WantTextInput) return 0;
        }
    }
    return CallWindowProc(g_previousWndProc, window, message, wparam, lparam);
}

void FeedMouse() {
    ImGuiIO& io = ImGui::GetIO();

    // Only while this process is the one in front: otherwise moving the mouse
    // over another application would still drive the panel.
    DWORD pid = 0;
    if (HWND foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &pid);
    if (pid != GetCurrentProcessId()) {
        io.AddFocusEvent(false);
        return;
    }
    io.AddFocusEvent(true);

    // Polled rather than taken from WM_MOUSEMOVE: the mouse messages go to
    // whichever window is under the cursor, and the panel is drawn into a
    // topmost WS_EX_NOACTIVATE window that never becomes the focused one.
    // Polling also costs nothing here, because GTA2 does not use the mouse for
    // anything in game - there is no input to conflict with.
    POINT cursor;
    if (GetCursorPos(&cursor) && ScreenToClient(g_presentWindow, &cursor)) {
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
}

bool ContainsNoCase(const char* haystack, const char* needle) {
    const size_t n = strlen(needle);
    if (!n) return true;
    for (const char* p = haystack; *p; ++p) {
        if (_strnicmp(p, needle, n) == 0) return true;
    }
    return false;
}

const char* ErrorText(int code) {
    switch (code) {
        case 0: return "success";
        case 1: return "general failure";
        case 2: return "module not found";
        case 3: return "GetProcAddress failed";
        case 4: return "invalid arguments";
        default: return "see remix_c.h";
    }
}

// ---------------------------------------------------------------------------
// Scale
//
// ImGui's defaults are drawn for a 1080p screen: 13px text and 8px padding on a
// 4K display come out at half the physical size they were meant to have, which
// is what made this menu unusable there. One multiplier drives the font, the
// style metrics and every fixed size in this file.
//
// The font is rasterised at its final size rather than magnified, so a change
// means rebuilding the atlas and dropping the D3D texture built from it. That
// must happen outside a frame, which is why it is done at the top of the render
// call rather than where the slider is.
// ---------------------------------------------------------------------------

// Segoe UI where Windows has it, which is everywhere since Vista, and ImGui's
// built-in ProggyClean where it does not. The built-in is a 13px bitmap face and
// looks like one when rasterised at 26.
void LoadFont(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    char path[MAX_PATH] = "";
    if (GetWindowsDirectoryA(path, MAX_PATH)) {
        strncat(path, "\\Fonts\\segoeui.ttf", MAX_PATH - strlen(path) - 1);
        // Tested before the call rather than after: ImGui asserts on a font file
        // it cannot open, and an assert here is a message box over a full-screen
        // game.
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) path[0] = '\0';
    }
    if (path[0] && io.Fonts->AddFontFromFileTTF(path, floorf(16.0f * scale))) return;

    ImFontConfig cfg;
    cfg.SizePixels = floorf(13.0f * scale);
    io.Fonts->AddFontDefault(&cfg);
}

void ApplyScale(float scale) {
    g_scale = scale;
    g_resetWindowSize = true;   // the old size was in the old scale's pixels
    LoadFont(scale);
    ImGui::GetIO().Fonts->Build();
    ImGui_ImplDX9_InvalidateDeviceObjects();   // font texture rebuilt on the next frame

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();                      // from the defaults, not from the last scale
    ImGui::StyleColorsDark();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 14.0f;
    style.ScaleAllSizes(scale);
}

// The DPI the desktop is set to, as this process is allowed to see it.
//
// The distinction is the whole trick. Under RTX Remix the bridge client makes
// the process DPI-aware, so this is the real 240 of a 250% desktop and the panel
// has to do its own scaling. Without Remix nothing makes GTA2 aware, Windows
// reports a flat 96 and magnifies everything the game draws on the way out - so
// scaling here as well would multiply a magnification.
//
// GetDpiForWindow is the per-monitor answer and only exists from Windows 10
// 1607, so it is resolved at run time; GetDeviceCaps gives the same 96-or-real
// answer on anything older.
float DesktopDpi() {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow && g_presentWindow) {
        const UINT dpi = getDpiForWindow(g_presentWindow);
        if (dpi >= 48) return static_cast<float>(dpi);
    }
    if (HDC dc = GetDC(nullptr)) {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        if (dpi >= 48) return static_cast<float>(dpi);
    }
    return 96.0f;
}

// What to draw at when the scale is left on automatic: whatever the desktop's
// own scaling asks for, in quarter steps, but never more than the window it is
// drawn into can hold.
float AutoScale() {
    float s = DesktopDpi() / 96.0f;

    RECT rc = {0, 0, 0, 0};
    if (g_presentWindow && GetClientRect(g_presentWindow, &rc) && rc.bottom > rc.top) {
        // The panel is 600 units tall; anything that would not leave a margin
        // around it is too big however much DPI scaling asks for.
        const float fits = static_cast<float>(rc.bottom - rc.top) / 700.0f;
        if (s > fits) s = fits;
    }

    s = floorf(s * 4.0f + 0.5f) / 4.0f;
    if (s < 1.0f) s = 1.0f;
    if (s > 3.0f) s = 3.0f;
    return s;
}

// ---------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------

// Which of the three ini files a given setting lands in is an implementation
// detail nobody should have to remember, so one button saves all of them.
void DrawSaveBar() {
    if (ImGui::Button(SettingsDirty() ? "Save *" : "Save")) SettingsSaveAll();
    ImGui::SetItemTooltip("Writes gta2dx9_settings.ini, gta2dx9_lights.ini and "
                          "gta2dx9_effects.ini beside the game. All three are read at startup.");
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        SettingsLoadAll();
        LightsLoadOverrides();
        SyntheticLightsLoad(nullptr);
        LightsInvalidate(0);
    }
    ImGui::SetItemTooltip("Throws away unsaved changes and reads the files back.");

    ImGui::SameLine();
    if (SettingsDirty()) {
        ImGui::TextColored(kWarn, "unsaved changes");
    } else {
        ImGui::TextColored(kDim, "saved");
    }

    // Right-aligned, because it is a mode switch for the whole panel rather than
    // one more setting in the list.
    const float width = ImGui::CalcTextSize("Advanced").x + ImGui::GetFrameHeight()
                        + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SameLine();
    const float slack = ImGui::GetContentRegionAvail().x - width;
    if (slack > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack);
    ImGui::Checkbox("Advanced", &g_advanced);
    ImGui::SetItemTooltip("Shows the diagnostics and the tuning this port was built with.");
}

// Only speaks up when something is actually wrong. The running totals that used
// to live here are on the Diagnostics tab.
void DrawWarnings() {
    bool any = false;
    if (!RemixApiAvailable()) {
        ImGui::TextColored(kBad, "Remix API unavailable: %s", RemixApiStatusText());
        any = true;
    }
    if (!game::LightingEnabled()) {
        ImGui::TextColored(kBad, "GTA2's lighting is switched off, so it never lists a light and "
                                 "none can be injected. Turn lighting on in the GTA2 Manager.");
        any = true;
    }
    const RemixLightStats& st = LightsStats();
    if (st.tracked > 0 && st.drawnLastFrame == 0 && LightsSettings().enabled) {
        ImGui::TextColored(kBad, "Lights are being tracked but none reached Remix.");
        any = true;
    }
    if (st.applyFailures) {
        ImGui::TextColored(kBad, "CreateLight failed %u time(s), last code %d (%s)",
                           st.applyFailures, st.lastApplyError, ErrorText(st.lastApplyError));
        any = true;
    }
    if (any) ImGui::Separator();
}

// The one control both light systems share: does this light belong to the night?
//
// Set as two sun elevations rather than as clock times, because that is what
// really decides it - the same lamp comes on later in June than in December and
// at a different hour at a different latitude, and reading it off the sun gets
// all of that for free. The simple view is the checkbox; the two angles are a
// detail.
bool DrawGate(DaylightGate& gate, const char* what) {
    bool changed = ImGui::Checkbox("Off in daylight", &gate.enabled);
    if (!gate.enabled) return changed;

    ImGui::SameLine();
    const float now = DaylightGateFactor(gate);
    ImGui::TextColored(now > 0.99f ? kGood : (now < 0.01f ? kDim : kWarn), "%s %.0f%%", what,
                       now * 100.0f);
    if (!Adv()) return changed;

    // Side by side and unlabelled: five of these stack up in the map light table
    // and a row apiece each would make it three times as tall as it is worth.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(120.0f));
    changed |= ImGui::SliderFloat("##offabove", &gate.offAboveDeg, -20.0f, 20.0f, "out %.1f deg");
    ImGui::SetItemTooltip("Fully off once the sun is this high. 0 is the horizon.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(120.0f));
    changed |= ImGui::SliderFloat("##onbelow", &gate.onBelowDeg, -30.0f, 10.0f, "on %.1f deg");
    ImGui::SetItemTooltip("Fully on once the sun is this far down. -6 is the end of civil "
                          "twilight, when real street lighting is at full.");
    if (gate.offAboveDeg <= gate.onBelowDeg) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "the wrong way round");
        ImGui::SetItemTooltip("It switches hard instead of fading.");
    }
    return changed;
}

// ---------------------------------------------------------------------------
// General
// ---------------------------------------------------------------------------

void DrawGeneral() {
    ImGui::SeparatorText("Status");

    ImGui::TextColored(RemixApiAvailable() ? kGood : kBad, "Remix: %s", RemixApiStatusText());
    const char* scene = LightsScene();
    ImGui::TextColored(scene[0] ? kGood : kDim, "District: %s",
                       scene[0] ? scene : "not identified yet");

    ImGui::SeparatorText("Frame rate");

    // GTA2 advances its simulation exactly one step per rendered frame and
    // scales nothing by elapsed time, so the cap is also the speed of the game.
    // Its own pacer waits on a hardcoded 33 ms step (gta2.exe!0x0045A460 returns
    // 33) and the registry's max_frame_rate/min_frame_rate are read as plain
    // booleans, so the number has to be ours. There is no setting that gives 60
    // fps of motion at 1x speed - that would need the game's per-step constants
    // halved, which a renderer cannot do.
    float fps = FrameLimitFps();
    ImGui::SetNextItemWidth(S(260.0f));
    if (ImGui::SliderFloat("Frame cap", &fps, 0.0f, 240.0f, fps <= 0.0f ? "off" : "%.0f fps")) {
        FrameLimitSet(fps);
        SettingsMarkDirty();
    }
    ImGui::SameLine();
    const float capped = FrameLimitFps();
    if (capped <= 0.0f) {
        ImGui::TextColored(kWarn, "uncapped -- as fast as the machine runs");
    } else {
        ImGui::TextColored(kDim, "game speed %.2fx", capped / 30.3030f);
    }
    Help("GTA2 runs one simulation step per frame, so the cap is the speed of the game as well as "
         "of the picture. 30.30 fps is its original 33 ms step.");

    struct Preset { float fps; const char* label; };
    static const Preset presets[] = {
        {30.3030f, "30 (1.00x)"}, {45.0f, "45 (1.49x)"}, {60.0f, "60 (1.98x)"},
        {90.0f, "90 (2.97x)"},    {0.0f, "Uncapped"},
    };
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        if (ImGui::SmallButton(presets[i].label)) {
            FrameLimitSet(presets[i].fps);
            SettingsMarkDirty();
        }
        ImGui::PopID();
    }

    if (Adv()) {
        ImGui::Text("measured %.1f fps", FrameLimitMeasuredFps());
        const float idle = FrameLimitIdleFraction();
        ImGui::SameLine();
        if (FrameLimitFps() > 0.0f && idle < 0.02f) {
            ImGui::TextColored(kWarn, "-- nothing spent waiting, so the machine is the limit, not "
                                      "the cap");
        } else {
            ImGui::TextColored(kDim, "-- %.0f%% of each frame waiting on the cap", idle * 100.0f);
        }
    }

    ImGui::SeparatorText("Menu and HUD shape");

    // GTA2 lays its front end out at 640x480 and its HUD at whatever its options
    // screen holds - both 4:3 - so on a widescreen display one of the two has to
    // give: either the artwork keeps its shape and the display is wider than it,
    // or it is stretched a third wider than it was drawn. Which of those looks
    // right is taste, not correctness, so it is a switch rather than a decision
    // made here.
    const char* aspect = DisplayAspect();
    int fit = HudFit() == HudFitMode::Stretch ? 1 : 0;
    const int wasFit = fit;
    ImGui::RadioButton("4:3 (original)", &fit, 0);
    ImGui::SetItemTooltip("The shape GTA2 drew its menus and HUD in, centred on the display. A "
                          "circle on screen is a circle.");
    ImGui::SameLine();
    char stretched[64];
    _snprintf(stretched, sizeof(stretched) - 1, "%s (stretched)", aspect[0] ? aspect : "Widescreen");
    stretched[sizeof(stretched) - 1] = 0;
    ImGui::RadioButton(stretched, &fit, 1);
    ImGui::SetItemTooltip("Fills the display, which is what GTA2's own renderer did. On a 16:9 "
                          "screen it makes the 4:3 artwork a third too wide -- which plenty of "
                          "people prefer to bars down the sides.");
    if (fit != wasFit) {
        SetHudFit(fit ? HudFitMode::Stretch : HudFitMode::Fit);
        SettingsMarkDirty();
    }
    if (aspect[0] && !strcmp(aspect, "4:3")) {
        ImGui::TextColored(kDim, "This display is 4:3 already, so the two are the same.");
    }

    ImGui::SeparatorText("Menu size");

    bool automatic = g_scaleSetting <= 0.0f;
    if (ImGui::Checkbox("Match the display", &automatic)) {
        g_scaleSetting = automatic ? 0.0f : g_scale;
        SettingsMarkDirty();
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "currently %.2fx", g_scale);
    Help("The panel is sized for a 1080p screen at 1x. Automatic picks 2x on a 4K display.");
    if (!automatic) {
        ImGui::SetNextItemWidth(S(260.0f));
        if (ImGui::SliderFloat("Scale", &g_scaleSetting, 1.0f, 3.0f, "%.2fx")) SettingsMarkDirty();
    }
}

// ---------------------------------------------------------------------------
// Lighting -- the game's own lights
// ---------------------------------------------------------------------------

void DrawLighting() {
    RemixLightSettings& s = LightsSettings();
    const RemixLightStats& st = LightsStats();

    ImGui::Checkbox("Inject the game's lights", &s.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Street", &s.injectStatic);
    ImGui::SetItemTooltip("Lamps, neon, traffic lights -- everything from the map that never "
                          "moves.");
    ImGui::SameLine();
    ImGui::Checkbox("Moving", &s.injectMoving);
    ImGui::SetItemTooltip("Vehicle lamps, train lights, muzzle flashes.");

    ImGui::SeparatorText("Brightness and colour");

    // Radiance = colour x the game's intensity x brightness x the reach term.
    // GTA2 sets 96% of its map lights to intensity 1.0, so its own intensity
    // carries almost no variation and this slider is where the overall level
    // comes from.
    ImGui::SliderFloat("Brightness", &s.radianceScale, 0.1f, 1000.0f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    Help("The master control. GTA2's own intensities barely vary, so this is where the overall "
         "level comes from.");

    ImGui::SliderFloat("Moving lights", &s.movingScale, 0.0f, 20.0f, "%.2f");
    Help("Extra multiplier on headlights and flashes only -- they are small and brief and rarely "
         "want a street lamp's brightness.");

    // GTA2's palette is far more saturated than anything real - pure #FF8000
    // sodium, pure #00FFFF neon - and a path tracer bouncing that around
    // exaggerates it. Pulling each colour towards its own luminance desaturates
    // without darkening.
    ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f, "%.2f");
    Help("1.0 leaves GTA2's colours alone, 0 makes every light white. The game's palette is far "
         "more saturated than anything real.");

    ImGui::SeparatorText("What is lit, and when");

    // The kinds are a heuristic drawn from the maps rather than from a field:
    // the LGHT chunk carries no type at all (byte 13, which the format docs call
    // a shape field, is the blink jitter). Traffic signals are built at runtime
    // at intensity 200 with a cycling colour and vehicle lamps move with their
    // car; the rest split by hue, and across the shipped maps 12083 lights carry
    // 765 colours whose common ones are unmistakable.
    if (ImGui::BeginTable("kinds", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("in view");
        ImGui::TableSetupColumn("lit");
        ImGui::TableSetupColumn("daylight");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kGameLightClassCount; ++i) {
            GameLightClassSettings& gc = s.gameClass[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox(GameLightClassName(i), &gc.enabled)) SettingsMarkDirty();
            ImGui::TableNextColumn();
            ImGui::Text("%d", st.byClass[i]);
            ImGui::TableNextColumn();
            const int lit = st.litByClass[i];
            ImGui::TextColored(lit ? kGood : kDim, "%d", lit);
            ImGui::TableNextColumn();
            if (DrawGate(gc.gate, "lit")) SettingsMarkDirty();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kDim, "GTA2 has no day of its own, so each kind decides for itself whether "
                             "to go out at dawn.");

    if (!Adv()) return;

    ImGui::SeparatorText("Shape and reach");

    ImGui::SliderFloat("Emitter radius", &s.emitterRadius, 0.01f, 2.0f, "%.3f");
    ImGui::SetItemTooltip("Physical size of the light sphere, in map tiles. NOT the game's radius "
                          "-- that is its reach, and using it here would put a glowing ball the "
                          "width of the street around every lamp.");
    ImGui::SliderFloat("Reach exponent", &s.radiusExponent, 0.0f, 2.0f, "%.2f");
    ImGui::SetItemTooltip("How strongly the game's radius feeds brightness. 0 ignores it, 1 is "
                          "linear, 2 is closer to physical and very wide-ranging.");
    ImGui::SliderFloat("Reference reach", &s.referenceRadius, 0.5f, 8.0f, "%.2f");
    ImGui::SetItemTooltip("The reach that lands exactly on the brightness above. 3 tiles is the "
                          "commonest radius in the shipped maps.");
    ImGui::SliderFloat("Intensity floor", &s.minIntensity, 0.0f, 0.2f, "%.4f");
    ImGui::SetItemTooltip("Dimmer lights are dropped. GTA2 blinks a light by taking its intensity "
                          "to zero, so without a floor a traffic light glows red, amber and green "
                          "at once.");

    ImGui::Checkbox("Ambient fill light", &s.ambientFill);
    ImGui::SetItemTooltip("Stands a dim overhead distant light in for GTA2's constant ambient "
                          "term, which a path tracer has no equivalent for. An approximation, not "
                          "what the game did, so it is off by default.");
    if (s.ambientFill) {
        ImGui::SliderFloat("Ambient fill scale", &s.ambientFillScale, 0.0f, 20.0f, "%.2f");
        ImGui::SameLine();
        ImGui::TextColored(kDim, "game ambient %.3f", LightsAmbient());
    }

    if (ImGui::Button("Redefine all lights")) LightsInvalidate(0);
    ImGui::SetItemTooltip("Rebuilds every light in Remix from the current settings.");
}

// ---------------------------------------------------------------------------
// Extra lights -- the ones GTA2 never had
// ---------------------------------------------------------------------------

// Colour, brightness and reach for one invented category; the caps, the gate
// detail and the headlight beam geometry are advanced-only.
void DrawCategory(int category) {
    SyntheticCategorySettings& c = SyntheticLightsSettings().category[category];
    ImGui::PushID(category);

    bool changed = ImGui::Checkbox("On", &c.enabled);
    ImGui::SameLine();
    changed |= ImGui::ColorEdit3("Colour", c.rgb, ImGuiColorEditFlags_NoInputs);

    changed |= ImGui::SliderFloat("Brightness", &c.intensity, 0.0f, 8.0f, "%.3f",
                                  ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Reach (tiles)", &c.radius, 0.1f, 12.0f, "%.2f");
    if (category != kSynthHeadlight) {
        changed |= ImGui::SliderFloat("Flicker", &c.flicker, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("How far brightness wanders, 0 for a steady light. Each light "
                              "flickers on its own, so a fire never pulses in step.");
    }
    changed |= DrawGate(c.gate, "burning");

    if (category == kSynthHeadlight) {
        changed |= ImGui::SliderFloat("Beam angle", &c.coneAngleDeg, 5.0f, 90.0f, "%.1f deg");
        changed |= ImGui::SliderFloat("Beam pitch", &c.pitchDegrees, 0.0f, 45.0f, "%.1f deg");
    }

    if (Adv()) {
        changed |= ImGui::SliderFloat("Height offset", &c.heightOffset, -0.5f, 2.0f, "%.3f");
        changed |= ImGui::SliderInt("Max lights per frame", &c.maxLights, 1, 128);
        ImGui::SetItemTooltip("Every light is a CreateLight across the 32-bit Remix bridge and "
                              "some particle types arrive in bursts of dozens.");

        if (category == kSynthHeadlight) {
            changed |= ImGui::SliderFloat("Forward from centre", &c.forwardOffset, 0.0f, 2.0f,
                                          "%.3f");
            changed |= ImGui::SliderFloat("Beam separation", &c.sideOffset, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Cone softness", &LightsSettings().coneSoftness, 0.0f,
                                          1.0f, "%.2f");

            // Nothing here reads the car's real width. The model id is available
            // so a value can be stored per model, but it is a number you typed
            // rather than anything measured - which is why one setting for every
            // car stays the default.
            ImGui::SeparatorText("Per car model");
            if (ImGui::Checkbox("Use per-model beams", &SyntheticLightsSettings().perModelBeams)) {
                SyntheticLightsMarkDirty();
            }
            ImGui::SetItemTooltip("Off by default: these are typed numbers, not measured widths. "
                                  "0 on a field means use the value above.");
            if (ImGui::BeginTable("beams", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, S(220.0f)))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthFixed, S(56.0f));
                ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, S(52.0f));
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(56.0f));
                ImGui::TableSetupColumn("cone", ImGuiTableColumnFlags_WidthFixed, S(150.0f));
                ImGui::TableSetupColumn("separation", ImGuiTableColumnFlags_WidthFixed, S(150.0f));
                ImGui::TableSetupColumn("nose offset");
                ImGui::TableHeadersRow();
                for (const VehicleModelInfo& m : SyntheticVehicleModels()) {
                    ImGui::PushID(m.model);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(m.driven ? kGood : kDim, "%d", m.model);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%d", m.driven, m.seen);

                    ImGui::TableNextColumn();
                    const bool lit = SyntheticHighlightedModel() == m.model;
                    if (lit) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.75f, 1.0f));
                    }
                    if (ImGui::SmallButton(lit ? "lit" : "light")) {
                        SyntheticHighlightModel(lit ? -1 : m.model);
                    }
                    if (lit) ImGui::PopStyleColor();

                    BeamOverride* b = SyntheticFindBeam(m.model);
                    BeamOverride edit = b ? *b : BeamOverride();
                    bool touched = false;
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1);
                    touched |= ImGui::SliderFloat("##cone", &edit.coneAngleDeg, 0.0f, 90.0f,
                                                  "%.0f deg");
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1);
                    touched |= ImGui::SliderFloat("##side", &edit.sideOffset, 0.0f, 1.0f, "%.3f");
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1);
                    touched |= ImGui::SliderFloat("##fwd", &edit.forwardOffset, 0.0f, 2.0f,
                                                  "%.3f");
                    if (touched) SyntheticEditBeam(m.model) = edit;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    if (changed) SyntheticLightsMarkDirty();
    ImGui::PopID();
}

void DrawExtraLights() {
    SyntheticSettings& s = SyntheticLightsSettings();
    const SyntheticStats& st = SyntheticLightsStats();

    // GTA2 emits no light at all for gunfire, bullets, sparks or cigarettes, and
    // its headlamps are point lights with no beam. These are invented from the
    // game's live particle and vehicle lists. Which particle type is which
    // effect is written down nowhere in the game, so it is bound by hand on the
    // Diagnostics tab.
    if (ImGui::Checkbox("Add lights GTA2 never had", &s.enabled)) SyntheticLightsMarkDirty();
    Help("Gunfire, fires, sparks and headlight beams. GTA2 emits no light for any of them; these "
         "are invented from its live particle and vehicle lists.");

    if (!s.enabled) return;

    ImGui::Spacing();
    if (ImGui::BeginTabBar("categories")) {
        for (int i = 0; i < kSynthCategoryCount; ++i) {
            if (!ImGui::BeginTabItem(SyntheticCategoryName(i))) continue;

            int boundTypes = 0;
            for (int t = 0; t < kMaxParticleType; ++t) {
                if (SyntheticTypeBinding(t) == i) ++boundTypes;
            }
            if (i == kSynthHeadlight) {
                // A car counts as driven when someone is sitting in it -
                // vehicle+0x54, a pointer on every moving car and on no parked
                // one.
                bool changed = ImGui::Checkbox("Only cars with a driver", &s.drivenNeedsDriver);
                if (Adv()) {
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("...or just moving", &s.drivenAllowsMovement);
                    ImGui::SetItemTooltip("Fallback for a car being pushed. The occupant test "
                                          "already covers one stopped at a red light.");
                    if (s.drivenAllowsMovement) {
                        int window = static_cast<int>(s.drivenWindowMs);
                        changed |= ImGui::SliderInt("Movement window (ms)", &window, 0, 10000);
                        s.drivenWindowMs = static_cast<unsigned>(window);
                        changed |= ImGui::SliderFloat("Movement threshold", &s.drivenMinMovement,
                                                      0.0f, 0.05f, "%.4f");
                    }
                }
                if (changed) SyntheticLightsMarkDirty();
            } else if (boundTypes == 0) {
                ImGui::TextColored(kWarn, "No particle type is bound to this, so it emits "
                                          "nothing. Bind one on the Diagnostics tab.");
            }
            if (Adv()) {
                ImGui::TextColored(kDim, "emitted last frame %d, %u since start", st.emitted[i],
                                   st.totalEmitted[i]);
                if (st.capped[i]) {
                    ImGui::SameLine();
                    ImGui::TextColored(kWarn, "(%d dropped at the ceiling)", st.capped[i]);
                }
            }
            ImGui::Spacing();
            DrawCategory(i);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (Adv()) {
        ImGui::Separator();
        ImGui::TextColored(kDim, "particles walked %d, vehicles %d (%d driven)",
                           st.particlesWalked, st.vehiclesWalked, st.vehiclesDriven);
        if (!st.particleListFound) {
            ImGui::TextColored(kWarn, "No particle list yet -- normal in the menus.");
        }
        if (!st.vehicleListFound) ImGui::TextColored(kWarn, "No vehicle list yet.");
    }
}

// ---------------------------------------------------------------------------
// Time of day
// ---------------------------------------------------------------------------

void HourText(float hour, char* out, size_t size) {
    int h = static_cast<int>(hour);
    int m = static_cast<int>((hour - h) * 60.0f + 0.5f);
    if (m >= 60) { m -= 60; ++h; }
    if (h >= 24) h -= 24;
    _snprintf(out, size - 1, "%02d:%02d", h, m);
    out[size - 1] = '\0';
}

void DrawTimeOfDay() {
    TimeOfDaySettings& tod = TimeOfDay();

    // Remix places its sun from rtx.atmosphere.sunElevation and
    // rtx.atmosphere.sunRotation, both documented as game-drivable per frame and
    // both pushed through the Remix API's SetConfigVariable. The angles come
    // from the standard solar position equations for a latitude and a season, so
    // the sun rises in the east, is highest at local noon, and spends the night
    // as far below the horizon as it really would - which is what makes 2am dark
    // rather than merely dim.
    if (ImGui::Checkbox("Run a day/night cycle", &tod.enabled)) SettingsMarkDirty();
    Help("Drives Remix's sun from real solar geometry for the latitude and season below. Off "
         "leaves whatever sun rtx.conf last set.");

    if (!RemixApiAvailable()) {
        ImGui::TextColored(kWarn, "The clock runs, but nothing reaches the sky until Remix "
                                  "answers.");
    }

    char clock[16];
    HourText(TimeOfDayHour(), clock, sizeof(clock));
    float elevation = 0.0f, rotation = 0.0f;
    TimeOfDayAngles(&elevation, &rotation);

    // The civil/nautical/astronomical twilight boundaries, which is the honest
    // way to say how dark it is rather than "night".
    const char* phase = elevation > 0.0f     ? "day"
                        : elevation > -6.0f  ? "civil twilight"
                        : elevation > -12.0f ? "nautical twilight"
                        : elevation > -18.0f ? "astronomical twilight"
                                             : "night";

    ImGui::SeparatorText("Clock");
    float hour = TimeOfDayHour();
    ImGui::SetNextItemWidth(S(300.0f));
    if (ImGui::SliderFloat("##time", &hour, 0.0f, 24.0f, clock)) TimeOfDaySetHour(hour);
    ImGui::SameLine();
    ImGui::TextColored(elevation > 0.0f ? kWarn : kDim, "%s", phase);
    if (Adv()) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, "(sun %+.2f deg up, %.2f deg round)", elevation, rotation);
    }

    if (ImGui::Button("Midnight")) TimeOfDaySetHour(0.0f);
    ImGui::SameLine();
    if (ImGui::Button("Dawn")) TimeOfDaySetHour(6.0f);
    ImGui::SameLine();
    if (ImGui::Button("Noon")) TimeOfDaySetHour(12.0f);
    ImGui::SameLine();
    if (ImGui::Button("Dusk")) TimeOfDaySetHour(18.0f);
    ImGui::SameLine();
    if (ImGui::Checkbox("Paused", &tod.paused)) SettingsMarkDirty();

    ImGui::SetNextItemWidth(S(300.0f));
    if (ImGui::SliderFloat("Speed", &tod.minutesPerSecond, 0.1f, 60.0f, "%.2f game min/sec",
                           ImGuiSliderFlags_Logarithmic)) {
        SettingsMarkDirty();
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "a full day in %.1f real minutes",
                       tod.minutesPerSecond > 0.0f ? 1440.0f / tod.minutesPerSecond / 60.0f : 0.0f);

    char startText[16];
    HourText(tod.startHour, startText, sizeof(startText));
    ImGui::SetNextItemWidth(S(300.0f));
    if (ImGui::SliderFloat("Level starts at", &tod.startHour, 0.0f, 24.0f, startText)) {
        SettingsMarkDirty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Back to it")) TimeOfDayReset();

    if (!Adv()) return;

    // Anywhere City is nowhere in particular, so this is a choice rather than a
    // fact. Latitude sets how high the sun climbs and how steeply it rises;
    // declination sets the season (+23.4 in June, 0 at the equinoxes, -23.4 in
    // December). The defaults are a temperate northern latitude at the equinox.
    ImGui::SeparatorText("Where and when on Earth");
    bool changed = false;
    changed |= ImGui::SliderFloat("Latitude", &tod.latitudeDeg, -66.0f, 66.0f, "%.1f deg");
    ImGui::SetItemTooltip("Positive is north. Past about 66 the sun stops rising or setting at "
                          "the solstices.");
    changed |= ImGui::SliderFloat("Season", &tod.declinationDeg, -23.44f, 23.44f, "%.2f deg");
    ImGui::SetItemTooltip("Solar declination. +23.44 is midsummer in the north, 0 an equinox, "
                          "-23.44 midwinter.");
    struct Season { float declination; const char* label; };
    static const Season seasons[] = {
        {23.44f, "June solstice"}, {0.0f, "Equinox"}, {-23.44f, "December solstice"},
    };
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        if (ImGui::SmallButton(seasons[i].label)) {
            tod.declinationDeg = seasons[i].declination;
            changed = true;
        }
        ImGui::PopID();
    }

    // Remix's reference for sunRotation is not documented and GTA2's streets do
    // not have to run north-south anyway, so the bearing can be turned and, if
    // it comes out mirrored, flipped.
    ImGui::SeparatorText("Fitting it to the city");
    changed |= ImGui::SliderFloat("Compass offset", &tod.rotationOffsetDeg, -180.0f, 180.0f,
                                  "%.1f deg");
    changed |= ImGui::Checkbox("Sun travels clockwise from above", &tod.rotationClockwise);
    ImGui::SetItemTooltip("Which it does in the northern hemisphere. Uncheck if the sun rises "
                          "where it should set.");
    changed |= ImGui::SliderFloat("Elevation offset", &tod.elevationOffsetDeg, -30.0f, 30.0f,
                                  "%.1f deg");
    ImGui::SetItemTooltip("A thumb on the scale for pulling the night up out of pitch black.");
    changed |= ImGui::SliderFloat("Push interval", &tod.pushIntervalMs, 0.0f, 500.0f, "%.0f ms");
    ImGui::SetItemTooltip("Each push is a round trip across the 32-bit Remix bridge and the sun "
                          "moves a fraction of a degree per frame, so there is nothing to gain "
                          "from doing it every frame.");
    if (changed) SettingsMarkDirty();

    ImGui::TextColored(TimeOfDayPushed() ? kGood : kWarn, "%s", TimeOfDayStatus());
    ImGui::SameLine();
    ImGui::TextColored(kDim, "(%d pushes)", TimeOfDayPushCount());

    ImGui::SeparatorText("The day, hour by hour");
    if (ImGui::BeginTable("tod", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("time");
        ImGui::TableSetupColumn("elevation");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int h = 0; h < 24; h += 2) {
            float e = 0.0f, r = 0.0f;
            TimeOfDaySunAt(static_cast<float>(h), tod, &e, &r);
            ImGui::PushID(h);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[16];
            _snprintf(label, sizeof(label) - 1, "%02d:00", h);
            label[sizeof(label) - 1] = '\0';
            if (ImGui::SmallButton(label)) TimeOfDaySetHour(static_cast<float>(h));
            ImGui::TableNextColumn();
            ImGui::TextColored(e > 0.0f ? kWarn : kDim, "%+7.2f", e);
            ImGui::TableNextColumn();
            // A bar, so the shape of the day is visible without a plot. The
            // middle of it is the horizon.
            const int filled = static_cast<int>((e + 90.0f) / 180.0f * 40.0f + 0.5f);
            char bar[48];
            for (int i = 0; i < 40; ++i) bar[i] = i < filled ? '#' : '.';
            bar[40] = '\0';
            ImGui::TextColored(e > 0.0f ? kWarn : kDim, "%s", bar);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

void DrawSprites() {
    EffectSpriteSettings& fx = EffectSprites();

    // Fire and explosion sprites had a black rim that no amount of alpha work
    // would shift, because the black is not behind the alpha at all: the artwork
    // fades to black on its way out to the cutout edge and those texels are
    // fully opaque. Art drawn that way only makes sense additively, where black
    // adds nothing - and GTA2's own renderers never blended, so on a CRT the rim
    // was simply lived with. Which sprites those are is read off the artwork,
    // not off a list.
    ImGui::SeparatorText("Fire and explosions");
    int mode = fx.mode == EffectSpriteMode::Additive ? 2
                                                     : (fx.mode == EffectSpriteMode::Cutout ? 1 : 0);
    const int was = mode;
    ImGui::RadioButton("Glow", &mode, 2);
    ImGui::SetItemTooltip("Drawn additively, which is what the artwork was made for. Remix treats "
                          "additive draws as emissive, so a fireball lights the street.");
    ImGui::SameLine();
    ImGui::RadioButton("Fade the fringe", &mode, 1);
    ImGui::SetItemTooltip("Turns the artwork's brightness into alpha and keeps the alpha test. No "
                          "glow, but the geometry stays opaque to the path tracer.");
    ImGui::SameLine();
    ImGui::RadioButton("Leave alone", &mode, 0);
    ImGui::SetItemTooltip("The original behaviour, black rim included.");
    if (mode != was) {
        fx.mode = mode == 2 ? EffectSpriteMode::Additive
                            : (mode == 1 ? EffectSpriteMode::Cutout : EffectSpriteMode::Off);
        RebuildDeviceTextures();
        SettingsMarkDirty();
    }

    ImGui::SeparatorText("How much of the world GTA2 keeps");

    // GTA2 sizes its own view rectangle 4:3 whatever the screen is, and every
    // "is this on screen" question goes to it. See
    // WorldView::MatchGameViewToFrame.
    bool offscreen = SpawnOffscreen();
    if (ImGui::Checkbox("Widen GTA2's view to the frame", &offscreen)) {
        SetSpawnOffscreen(offscreen);
        SettingsMarkDirty();
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, SpawnOffscreenActive() ? "gta2.exe patched" : "gta2.exe as shipped");
    Help("GTA2 works out what is on screen as a 4:3 rectangle whatever the screen really is, and "
         "creates and removes cars and pedestrians just outside it - which in a 16:9 frame of the "
         "same height is inside our left and right edges, so they appear out of nothing. This "
         "makes that rectangle the shape of the frame we draw, at the one place the game builds "
         "it, so spawning, recycling and drawing all move out together. Turning it off puts "
         "gta2.exe back byte for byte, here and now, without a restart. Persist it with "
         "spawn_offscreen under [renderer].");

    ImGui::SeparatorText("Standing on the ground");

    // GTA2 puts all four corners of a sprite's quad at the single level of the
    // object underneath and never depth tested, so a flat quad lying in the
    // floor was fine. As real geometry it is not: on a ramp a horizontal quad
    // cuts through the lid and the uphill half vanishes. The lid under the four
    // corners is sampled, a plane is fitted, and that plane becomes the sprite's
    // transform. The quad itself is untouched - baking corner heights into the
    // vertices would change the object-space shape as a car climbed a ramp, and
    // that shape is exactly what Remix recognises a sprite by.
    bool conform = SpriteConform();
    if (ImGui::Checkbox("Tilt sprites onto the ground", &conform)) SetSpriteConform(conform);
    Help("Stands each sprite on a plane fitted to the map under it, so it stays parallel to a ramp "
         "instead of cutting through it.");

    float height = SpriteHeight();
    ImGui::SetNextItemWidth(S(300.0f));
    if (ImGui::SliderFloat("Ride height", &height, 0.0f, 1.0f, "%.3f blocks")) {
        SetSpriteHeight(height);
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%.0f cm", SpriteHeight() * 200.0f);
    // A flat quad on the ground casts no shadow a path tracer can show - the
    // shadow lands exactly where the quad already is. Both the shadow and the
    // contact darkening appear only once the sprite has real height. The cost is
    // parallax, about h/3 at the edge of the screen, which is why the default is
    // small.
    Help("How high the object a sprite stands in for sits above the road. A quad flat on the "
         "ground casts no shadow a path tracer can show; too much and it reads as hovering.");

    float roll = SpriteRoll();
    ImGui::SetNextItemWidth(S(300.0f));
    if (ImGui::SliderFloat("Sideways tilt", &roll, 0.0f, 1.0f, "%.2f")) SetSpriteRoll(roll);
    // Using the whole fitted plane is right for a rigid body and wrong for a
    // car: one crossing a ramp at an angle would roll and lift a corner, which
    // cars on wheels do not do. The pitch along the direction of travel is
    // always kept whole; this scales only the roll.
    Help("0 keeps cars level side to side, 1 uses the whole fitted plane. The pitch along the "
         "direction of travel is always kept.");

    if (!Adv()) return;

    ImGui::SeparatorText("Effect sprite thresholds");
    ImGui::Text("%d of %d built frames classified as effects; %d drawn additively last frame",
                EffectSpriteFrames(), ClassifiedFrames(), EffectBatchesDrawn());
    bool changed = false;
    changed |= ImGui::SliderFloat("Fringe darker than", &fx.edgeLuma, 0.0f, 64.0f, "%.0f");
    ImGui::SetItemTooltip("Median luminance of the opaque texels touching the hole. Fire and "
                          "explosion frames sit at about 5; the darkest thing that is not an "
                          "effect, a pedestrian in shadow, sits at 23.");
    changed |= ImGui::SliderFloat("...or all of it darker than", &fx.faintLuma, 0.0f, 128.0f,
                                  "%.0f");
    ImGui::SetItemTooltip("The brightest texel anywhere in the sprite. An explosion's last frames "
                          "are embers and smoke, dark all over with no crisp outline left. The "
                          "darkest pedestrian peaks at 105. 0 turns this route off.");
    if (fx.mode == EffectSpriteMode::Cutout) {
        changed |= ImGui::SliderFloat("Fade gain", &fx.cutoutGain, 0.5f, 8.0f, "%.2f");
        ImGui::SetItemTooltip("alpha = luminance x this.");
    }
    if (changed) SettingsMarkDirty();
    if (ImGui::Button("Rebuild textures now")) RebuildDeviceTextures();
    ImGui::SetItemTooltip("Classification happens when a texture is built, so a threshold change "
                          "only reaches artwork that is rebuilt.");

    ImGui::SeparatorText("Holes in the artwork");
    // GTA2's artists drew outlines in the colour-key index, so there are slits
    // straight through solid walls. The original showed the black background
    // behind them; a path tracer shows the sky. See CloseArtworkHoles.
    bool close = CloseHoles();
    if (ImGui::Checkbox("Close slits in the artwork", &close)) {
        SetCloseHoles(close);
        RebuildDeviceTextures();
        SettingsMarkDirty();
    }
    ImGui::SetItemTooltip("A transparent island that reaches no edge of the tile and is at most "
                          "two texels across is an outline, not a hole. Windows and grilles are "
                          "thicker than that and are left alone.");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%d texel(s) closed", HolesClosed());
    if (close) {
        int maxIsland = CloseHoleMax();
        if (ImGui::SliderInt("...and any island up to", &maxIsland, 0, 64, "%d texels")) {
            SetCloseHoleMax(maxIsland);
            RebuildDeviceTextures();
            SettingsMarkDirty();
        }
        ImGui::SetItemTooltip("For the slits too wide for the shape test. A long slit and a small "
                              "window pane are the same shape, so this trades one mistake for the "
                              "other -- raise it until a window disappears, then back off. 0 is "
                              "the shape test alone.");
    }

    ImGui::SeparatorText("Alpha");
    // GTA2's artwork is palettised with entry 0 as a colour key, so every edge
    // is binary and there is no real alpha to blend. The black rim that used to
    // show was the colour behind the key, which every filter - above all the
    // mipmaps Remix builds - averaged into its neighbours. Those texels now
    // carry their neighbours' colour instead.
    int alpha = gta2::GetAlphaMode() == gta2::AlphaMode::Blend ? 1 : 0;
    if (ImGui::RadioButton("Alpha test", &alpha, 0)) gta2::SetAlphaMode(gta2::AlphaMode::Test);
    ImGui::SetItemTooltip("The default. A path tracer wants a cutout to be a cutout -- a blended "
                          "surface has no single depth for a ray to hit.");
    ImGui::SameLine();
    if (ImGui::RadioButton("Alpha blend", &alpha, 1)) gta2::SetAlphaMode(gta2::AlphaMode::Blend);
    int ref = gta2::GetAlphaRef();
    if (ImGui::SliderInt("Cutoff", &ref, 1, 254)) gta2::SetAlphaRef(ref);
    ImGui::SetItemTooltip("Where the binary cutoff falls, in alpha test mode.");

    float feather = SpriteFeather();
    if (ImGui::SliderFloat("Sprite edge softness", &feather, 0.0f, 1.0f, "%.2f")) {
        SetSpriteFeather(feather);
    }
    ImGui::SetItemTooltip("Invents the ramp 1-bit alpha never had by dimming each texel by how "
                          "much of its neighbourhood is empty. Sprites only, and it takes effect "
                          "on textures built after the change.");

    // A GTA2 car is not one sprite: body, then lights, then any logo, each a
    // separate quad at exactly the same height. The game painted them in order;
    // as real geometry they are coplanar and the depth test picks a winner per
    // pixel, which is the flicker. Each sprite landing where another already is
    // this frame goes one step higher, so the painter's order becomes a real
    // stacking order.
    float step = SpriteStackStep();
    if (ImGui::SliderFloat("Stacked sprite step", &step, 0.0f, 0.05f, "%.4f blocks")) {
        SetSpriteStackStep(step);
    }
    ImGui::SetItemTooltip("0 puts a car's body, lights and logo back on one plane, which is what "
                          "makes them flicker.");

    ImGui::SeparatorText("The world's floor");
    // GTA2's map is not a closed solid, so anything looking down through a gap
    // in it used to find the sky. See SealTileIndex in world_mesh.h.
    bool seal = gta2::WorldSeal();
    if (ImGui::Checkbox("Black floor under the city", &seal)) {
        gta2::SetWorldSeal(seal);
        SettingsMarkDirty();
    }
    ImGui::SetItemTooltip("One black quad well below the lowest block and reaching past the map "
                          "on every side, so a hole in the map shows black rather than the sky. "
                          "Built with the world, so this takes effect on the next level load.");

    ImGui::SeparatorText("Ground fit");
    const LiveGeometry::Conform& c = SpriteConformCounts();
    ImGui::Text("last frame: %d grounded, %d airborne, %d without a floor, %d tilt-capped, "
                "%d needing clearance",
                c.grounded, c.airborne, c.noGround, c.capped, c.straddled);
    ImGui::TextColored(kDim, "worst gap to the floor %.3f blocks, worst step across a footprint "
                             "%.3f", c.maxGap, c.maxResidual);
    ImGui::SetItemTooltip("The gap is the check on the whole scheme: on a street with nothing in "
                          "the air it should sit near zero, which is the game and the map "
                          "agreeing about which floor a sprite is on.");
    int shapes = 0, shapeTextures = 0;
    SpriteShapeCounts(&shapes, &shapeTextures);
    ImGui::Text("%d distinct object-space quad(s) across %d sprite texture(s)", shapes,
                shapeTextures);
    ImGui::SetItemTooltip("Should climb for a few seconds after a level loads and then sit still. "
                          "If it keeps climbing, some sprite's quad is dithering and Remix cannot "
                          "track it between frames.");

    // Used where there is no floor to measure - off the map, or over a hole -
    // and, with conforming off, on every sprite. 72% of the surfaces a sprite
    // can stand on in the shipped districts are flat, 21.9% are 7 degree ramps,
    // 5.9% are 26 degree: no single number covers both a pedestrian and a car,
    // which is why this is only the fallback.
    float lift = SpriteLift();
    if (ImGui::SliderFloat("Fallback lift", &lift, 0.0f, 0.5f, "%.3f blocks")) SetSpriteLift(lift);
    ImGui::SetItemTooltip("Clearance used where there is no floor to measure, and on every sprite "
                          "when tilting is off.");
    if (ImGui::Button("Reset heights to defaults")) {
        SetSpriteHeight(kDefaultSpriteHeight);
        SetSpriteLift(kDefaultSpriteLift);
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

const RemixTrackedLight* FindTracked(uint64_t key) {
    if (!key) return nullptr;
    for (const RemixTrackedLight& t : LightsTracked()) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

bool PassesFilter(const RemixTrackedLight& t) {
    if (!(t.moving ? g_showMoving : g_showStatic)) return false;
    if (!g_showDark && t.def.intensity < LightsSettings().minIntensity) return false;
    if (g_onlyOverridden && !LightsFindOverride(t.key)) return false;
    if (g_onlyUndrawn && t.drawn) return false;
    if (g_filter[0]) {
        char text[48];
        _snprintf(text, sizeof(text) - 1, "%016llX", static_cast<unsigned long long>(t.key));
        text[sizeof(text) - 1] = '\0';
        if (!ContainsNoCase(text, g_filter)) return false;
    }
    return true;
}

void DrawLightStats() {
    const RemixLightStats& st = LightsStats();
    ImGui::Text("submitted by the game %d, tracked %d (%d moving), drawn last frame %d",
                st.submittedLastFrame, st.tracked, st.moving, st.drawnLastFrame);
    ImGui::Text("suppressed: %d by override, %d by kind, %d below the intensity floor",
                st.suppressedByOverride, st.suppressedByClass, st.suppressedByIntensity);
    if (st.tracked > 0) {
        ImGui::Text("game intensity in view %.2f..%.2f, %d of %d at full", st.minIntensitySeen,
                    st.maxIntensitySeen, st.atFullIntensity, st.tracked);
        ImGui::SetItemTooltip("Mostly 1.00 is expected: 96%% of the map lights in the shipped "
                              "districts are at full intensity. The real variation is in reach "
                              "and colour.");
    }
    ImGui::Text("created %u, redefined %u, destroyed %u", st.created, st.updated, st.destroyed);
    ImGui::TextColored(kDim, "light lists seen %u, %u frame(s) since the last one", st.collectsSeen,
                       st.framesSinceCollect);
    ImGui::Text("game ambient %.3f", LightsAmbient());
    ImGui::SetItemTooltip("GTA2's additive brightness floor, from gbh_SetAmbient. At 1.0 the "
                          "original renderer skipped lighting altogether -- its daylight fast "
                          "path.");
}

void DrawLightTable() {
    ImGui::SetNextItemWidth(S(200.0f));
    ImGui::InputText("filter by key", g_filter, sizeof(g_filter));
    ImGui::SameLine();
    ImGui::Checkbox("static", &g_showStatic);
    ImGui::SameLine();
    ImGui::Checkbox("moving", &g_showMoving);
    ImGui::SameLine();
    ImGui::Checkbox("blinked off", &g_showDark);
    ImGui::SameLine();
    ImGui::Checkbox("overridden", &g_onlyOverridden);
    ImGui::SameLine();
    ImGui::Checkbox("not drawn", &g_onlyUndrawn);

    const std::vector<RemixTrackedLight>& tracked = LightsTracked();

    // The tracked set is compacted with swap-and-pop, so its order jumps about
    // between frames. Sorted by identity, the list stays still under the cursor.
    std::vector<const RemixTrackedLight*> rows;
    rows.reserve(tracked.size());
    for (const RemixTrackedLight& t : tracked) {
        if (PassesFilter(t)) rows.push_back(&t);
    }
    std::sort(rows.begin(), rows.end(),
              [](const RemixTrackedLight* a, const RemixTrackedLight* b) {
                  if (a->key != b->key) return a->key < b->key;
                  return a->id < b->id;
              });

    ImGui::Text("%d of %d light(s)", static_cast<int>(rows.size()),
                static_cast<int>(tracked.size()));

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("lights", 8, flags, ImVec2(0.0f, S(300.0f)))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, S(140.0f));
    ImGui::TableSetupColumn("col", ImGuiTableColumnFlags_WidthFixed, S(30.0f));
    ImGui::TableSetupColumn("position (x, up, z)", ImGuiTableColumnFlags_WidthFixed, S(180.0f));
    ImGui::TableSetupColumn("int", ImGuiTableColumnFlags_WidthFixed, S(50.0f));
    ImGui::TableSetupColumn("reach", ImGuiTableColumnFlags_WidthFixed, S(55.0f));
    ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, S(64.0f));
    ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, S(95.0f));
    ImGui::TableSetupColumn("note");
    ImGui::TableHeadersRow();

    for (const RemixTrackedLight* t : rows) {
        ImGui::PushID(static_cast<int>(t->id));
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        char label[32];
        _snprintf(label, sizeof(label) - 1, "%016llX", static_cast<unsigned long long>(t->key));
        label[sizeof(label) - 1] = '\0';
        const bool selected = t->key != 0 && t->key == g_selected;
        if (ImGui::Selectable(t->key ? label : "(moving)", selected,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            g_selected = t->key;
        }

        ImGui::TableNextColumn();
        ImGui::ColorButton("##c", ImVec4(t->def.rgb[0], t->def.rgb[1], t->def.rgb[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(S(18.0f), S(18.0f)));

        ImGui::TableNextColumn();
        ImGui::Text("%7.2f %5.2f %7.2f", t->def.pos[0], t->def.pos[1], t->def.pos[2]);

        ImGui::TableNextColumn();
        ImGui::Text("%.2f", t->def.intensity);

        ImGui::TableNextColumn();
        ImGui::Text("%.2f", t->def.radius);

        ImGui::TableNextColumn();
        ImGui::TextColored(t->moving ? kWarn : kDim, "%s", t->moving ? "moving" : "static");

        ImGui::TableNextColumn();
        if (t->applyFailed) {
            ImGui::TextColored(kBad, "refused");
        } else if (t->drawn) {
            ImGui::TextColored(kGood, "drawn %u", t->updates);
        } else {
            ImGui::TextColored(kDim, "off");
        }

        ImGui::TableNextColumn();
        if (const RemixLightOverride* ov = LightsFindOverride(t->key)) {
            ImGui::TextColored(ov->disabled ? kBad : kWarn, "%s%s",
                               ov->disabled ? "disabled " : "",
                               ov->note.empty() ? "edited" : ov->note.c_str());
        } else {
            ImGui::TextUnformatted("");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawSelectedEditor() {
    if (!g_selected) {
        ImGui::TextColored(kDim, "Select a light to edit it. Moving lights have no stable "
                                 "identity and cannot be given a saved override -- use the "
                                 "moving-light brightness on the Lighting tab for those.");
        return;
    }

    ImGui::Text("Selected %016llX", static_cast<unsigned long long>(g_selected));
    const RemixTrackedLight* t = FindTracked(g_selected);
    ImGui::SameLine();
    if (t) {
        ImGui::TextColored(kDim, "-- colour %.2f %.2f %.2f, intensity %.2f, reach %.2f tiles",
                           t->def.rgb[0], t->def.rgb[1], t->def.rgb[2], t->def.intensity,
                           t->def.radius);
    } else {
        ImGui::TextColored(kDim, "-- not in view right now");
    }

    const bool identified = LightsIdentified() == g_selected;
    if (ImGui::Button(identified ? "Stop identifying" : "Identify (paint magenta)")) {
        LightsIdentify(identified ? 0 : g_selected);
    }
    ImGui::SetItemTooltip("Burns the light bright magenta so it can be found in the scene. Beats "
                          "a disable, so a light already switched off can still be located.");

    // Read through a copy so that merely looking at a light does not create a
    // row in the override file.
    const RemixLightOverride* existing = LightsFindOverride(g_selected);
    RemixLightOverride edit = existing ? *existing : RemixLightOverride();
    bool changed = false;

    changed |= ImGui::Checkbox("Disabled", &edit.disabled);
    changed |= ImGui::SliderFloat("Intensity x", &edit.intensity, 0.0f, 20.0f, "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::DragFloat3("Offset (tiles)", edit.offset, 0.01f, -8.0f, 8.0f, "%.3f");
    changed |= ImGui::Checkbox("Recolour", &edit.recolor);
    if (edit.recolor) {
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("##colour", edit.color);
    }
    changed |= ImGui::SliderFloat("Emitter radius (0 = global)", &edit.emitterRadius, 0.0f, 2.0f,
                                  "%.3f");

    char note[192] = "";
    strncpy(note, edit.note.c_str(), sizeof(note) - 1);
    if (ImGui::InputText("Note", note, sizeof(note))) {
        edit.note = note;
        changed = true;
    }

    if (changed) {
        LightsEditOverride(g_selected) = edit;
        LightsInvalidate(g_selected);
    }

    if (existing && ImGui::Button("Reset this light")) {
        LightsEraseOverride(g_selected);
        LightsInvalidate(g_selected);
    }
}

void DrawSavedOverrides() {
    ImGui::TextColored(kDim, "Every saved override, including ones belonging to other districts.");
    if (ImGui::Button("Clear all overrides")) {
        LightsClearOverrides();
        LightsInvalidate(0);
    }
    ImGui::SameLine();
    ImGui::TextColored(LightsOverridesDirty() ? kWarn : kDim, "%d in %s%s",
                       static_cast<int>(LightsOverrides().size()), LightsOverridePath(),
                       LightsOverridesDirty() ? " (unsaved)" : "");

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("overrides", 4, flags, ImVec2(0.0f, S(200.0f)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, S(150.0f));
    ImGui::TableSetupColumn("in view", ImGuiTableColumnFlags_WidthFixed, S(70.0f));
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, S(220.0f));
    ImGui::TableSetupColumn("note");
    ImGui::TableHeadersRow();

    int row = 0;
    for (const auto& entry : LightsOverrides()) {
        const RemixLightOverride& ov = entry.second;
        ImGui::PushID(row++);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        char label[32];
        _snprintf(label, sizeof(label) - 1, "%016llX",
                  static_cast<unsigned long long>(entry.first));
        label[sizeof(label) - 1] = '\0';
        if (ImGui::Selectable(label, entry.first == g_selected,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            g_selected = entry.first;
        }

        ImGui::TableNextColumn();
        const bool present = FindTracked(entry.first) != nullptr;
        ImGui::TextColored(present ? kGood : kDim, "%s", present ? "yes" : "no");

        ImGui::TableNextColumn();
        char intensity[24] = "";
        if (ov.intensity != 1.0f) {
            _snprintf(intensity, sizeof(intensity) - 1, "x%.2f ", ov.intensity);
        }
        char what[128];
        _snprintf(what, sizeof(what) - 1, "%s%s%s%s", ov.disabled ? "disabled " : "", intensity,
                  ov.recolor ? "recoloured " : "",
                  (ov.offset[0] || ov.offset[1] || ov.offset[2]) ? "moved " : "");
        what[sizeof(what) - 1] = '\0';
        ImGui::TextUnformatted(what);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(ov.note.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// Every particle type the game has spawned since the list was last cleared.
// GTA2 tags each particle with a type id but nothing names them, so this is how
// a category gets bound: clear the list, trigger one effect, and the id that
// appears is the one. The behaviour columns are what actually tell them apart -
// a bullet is far faster than anything else and travels alone, sparks arrive two
// dozen at once and are gone in three frames, a fire lasts ten times longer and
// clusters. Speed is tiles per frame, life is frames.
void DrawParticleTypes() {
    if (ImGui::Button("Clear list")) SyntheticForgetParticleTypes();
    ImGui::SameLine();
    if (ImGui::Button("Run structure probe")) SyntheticProbe("F4 menu");
    ImGui::SetItemTooltip("Writes an offset report to gta2dx9.log: which field in a particle or a "
                          "vehicle is the position, and which is the driver. Scored against the "
                          "camera and against which cars are moving.");
    ImGui::SameLine();
    const SyntheticStats& st = SyntheticLightsStats();
    ImGui::TextColored(kDim, "%d type(s) seen, %d particle(s) live",
                       static_cast<int>(SyntheticParticleTypes().size()), st.particlesWalked);
    ImGui::TextColored(kDim, "Trigger one effect at a time and bind the id that appears. Rows seen "
                             "in the last two seconds are green.");

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("types", 9, flags, ImVec2(0.0f, S(320.0f)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, S(54.0f));
    ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, S(46.0f));
    ImGui::TableSetupColumn("spawns", ImGuiTableColumnFlags_WidthFixed, S(62.0f));
    ImGui::TableSetupColumn("burst", ImGuiTableColumnFlags_WidthFixed, S(50.0f));
    ImGui::TableSetupColumn("speed", ImGuiTableColumnFlags_WidthFixed, S(64.0f));
    ImGui::TableSetupColumn("rise", ImGuiTableColumnFlags_WidthFixed, S(66.0f));
    ImGui::TableSetupColumn("life", ImGuiTableColumnFlags_WidthFixed, S(56.0f));
    ImGui::TableSetupColumn("height", ImGuiTableColumnFlags_WidthFixed, S(58.0f));
    ImGui::TableSetupColumn("bound to");
    ImGui::TableHeadersRow();

    const unsigned now = GetTickCount();
    for (const ParticleTypeInfo& info : SyntheticParticleTypes()) {
        ImGui::PushID(info.type);
        ImGui::TableNextRow();
        const bool recent = now - info.lastSeenTick < 2000;

        ImGui::TableNextColumn();
        ImGui::TextColored(recent ? kGood : kDim, "0x%02X", info.type);
        ImGui::TableNextColumn();
        ImGui::Text("%d", info.liveNow);
        ImGui::TableNextColumn();
        ImGui::Text("%u", info.spawns);
        ImGui::TableNextColumn();
        ImGui::Text("%d", info.maxBurst);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", info.meanSpeed);
        ImGui::TableNextColumn();
        ImGui::TextColored(info.meanRise > 0.002f ? kWarn : kDim, "%+.4f", info.meanRise);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", info.meanLife);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", info.meanHeight);

        ImGui::TableNextColumn();
        const bool lit = SyntheticHighlightedType() == info.type;
        if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.75f, 1.0f));
        if (ImGui::SmallButton(lit ? "lit" : "light")) {
            SyntheticHighlightType(lit ? -1 : info.type);
        }
        if (lit) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Paint every particle of this type magenta. Looking at the screen is "
                              "the only way to tell a fire from the smoke above it.");
        ImGui::SameLine();

        const int bound = SyntheticTypeBinding(info.type);
        for (int i = 0; i < kSynthCategoryCount; ++i) {
            if (i == kSynthHeadlight) continue;   // vehicles, not particles
            if (i) ImGui::SameLine();
            const bool on = bound == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.25f, 1.0f));
            if (ImGui::SmallButton(SyntheticCategoryName(i))) {
                SyntheticBindType(info.type, on ? -1 : i);
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// Every distinct frame built this session keeps the numbers it was judged on and
// the sprite pass records which frames it actually drew, so a sprite that still
// has a black rim is a row to look up rather than a guess.
void DrawTextureReport() {
    bool dumping = TextureDumping();
    if (ImGui::Checkbox("Dump each frame as a TGA", &dumping)) SetTextureDumping(dumping);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%d written to %s", TextureFramesDumped(), TextureDumpDir());
    if (ImGui::Button("Write the report now")) {
        const int rows = WriteTextureReport();
        Log("texture report: written from the menu, %d row(s)", rows);
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "gta2dx9_textures.csv, also written automatically on shutdown.");
}

void DrawDiagnostics() {
    if (ImGui::CollapsingHeader("Light injection", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawLightStats();
    }
    if (ImGui::CollapsingHeader("Lights in view")) {
        DrawLightTable();
        ImGui::Separator();
        DrawSelectedEditor();
    }
    if (ImGui::CollapsingHeader("Saved light overrides")) DrawSavedOverrides();
    if (ImGui::CollapsingHeader("Particle types")) DrawParticleTypes();
    if (ImGui::CollapsingHeader("Textures")) DrawTextureReport();
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

float& DebugMenuUiScale() { return g_scaleSetting; }

void DebugMenuInit(HWND inputWindow, HWND presentWindow, IDirect3DDevice9* device) {
    if (g_initialised || !device || !presentWindow) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // no imgui.ini left in the game folder
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Two mouse positions can be queued in a frame - the backend's and the one
    // fed by FeedMouse. Trickling would apply them on alternate frames instead of
    // letting the later one win, which turns a drag into an oscillation between
    // the two. With trickling off the whole queue resolves in one frame and the
    // polled position, queued last, is the one that stands.
    io.ConfigInputTrickleEventQueue = false;
    io.ConfigWindowsMoveFromTitleBarOnly = true;   // dragging a slider must not drag the window
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(presentWindow) || !ImGui_ImplDX9_Init(device)) {
        Log("F4 menu: ImGui backend init failed");
        ImGui::DestroyContext();
        return;
    }

    g_presentWindow = presentWindow;
    g_inputWindow = inputWindow;
    if (inputWindow) {
        g_previousWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(inputWindow, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(MenuWndProc)));
    }

    ApplyScale(g_scaleSetting > 0.0f ? g_scaleSetting : AutoScale());

    g_initialised = true;
    Log("F4 menu ready (input on %p, drawn into %p, scale %.2f at %.0f dpi)", inputWindow,
        presentWindow, g_scale, DesktopDpi());
}

void DebugMenuShutdown() {
    if (!g_initialised) return;
    if (g_inputWindow && g_previousWndProc) {
        SetWindowLongPtrA(g_inputWindow, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_previousWndProc));
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_initialised = false;
    g_visible = false;
    g_inputWindow = nullptr;
    g_presentWindow = nullptr;
    g_previousWndProc = nullptr;
}

void DebugMenuPoll() {
    if (!g_initialised) return;

    // Polled rather than taken off WM_KEYDOWN: GTA2 reads the keyboard through
    // DirectInput, and relying on the message reaching the window proc first has
    // no advantage here. Alt+F4 is excluded so closing the game still closes it.
    const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0 && !altDown;
    DWORD pid = 0;
    if (HWND foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &pid);
    if (f4Down && !g_f4Down && pid == GetCurrentProcessId()) Toggle();
    g_f4Down = f4Down;

    if (g_visible) FeedMouse();
}

bool DebugMenuVisible() { return g_visible; }

void DebugMenuRender() {
    if (!g_initialised || !g_visible) return;

    // Before NewFrame, because rebuilding the font atlas mid-frame is not
    // allowed. A window whose size changed moves the automatic scale too.
    const float wanted = g_scaleSetting > 0.0f ? g_scaleSetting : AutoScale();
    if (fabsf(wanted - g_scale) > 0.005f) ApplyScale(wanted);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Sized only when it has to be - on the first frame and after a scale
    // change - so a window the user has resized by hand stays that way.
    if (g_resetWindowSize) {
        g_resetWindowSize = false;
        ImGui::SetNextWindowSize(ImVec2(S(780.0f), S(600.0f)));
    }
    if (g_resetWindowPos) {
        g_resetWindowPos = false;
        ImGui::SetNextWindowPos(ImVec2(S(40.0f), S(40.0f)));
    }

    bool open = true;
    if (ImGui::Begin("GTA2 RTX Remix", &open, ImGuiWindowFlags_NoSavedSettings)) {
        DrawSaveBar();
        ImGui::Separator();
        DrawWarnings();

        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("General")) {
                DrawGeneral();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Lighting")) {
                DrawLighting();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Extra lights")) {
                DrawExtraLights();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Time of day")) {
                DrawTimeOfDay();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sprites")) {
                DrawSprites();
                ImGui::EndTabItem();
            }
            if (Adv() && ImGui::BeginTabItem("Diagnostics")) {
                DrawDiagnostics();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) Toggle();   // the title bar's close button, same as pressing F4

    // Anything the menu touched this frame is unsaved. Cheaper and more honest
    // than trying to mark every individual widget.
    if (ImGui::GetIO().WantCaptureMouse && ImGui::IsAnyItemActive()) SettingsMarkDirty();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace gta2dx9
