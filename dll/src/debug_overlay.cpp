#include "debug_overlay.h"

#include "frame_limiter.h"
#include "game_access.h"
#include "live_geometry.h"
#include "../../src/renderer.h"
#include "log.h"
#include "remix_api.h"
#include "remix_lights.h"
#include "settings.h"
#include "synthetic_lights.h"
#include "texture_store.h"
#include "time_of_day.h"

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

uint64_t g_selected = 0;
bool     g_resetWindowPos = true;

// Filters. A district with a hundred lamps in view is unusable without them.
char g_filter[64] = "";
bool g_showStatic = true;
bool g_showMoving = true;
bool g_showDark = true;
bool g_onlyOverridden = false;
bool g_onlyUndrawn = false;

const ImVec4 kGood = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kBad = ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
const ImVec4 kWarn = ImVec4(0.95f, 0.80f, 0.35f, 1.0f);
const ImVec4 kDim = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);

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

// The one button. Which of the three files a given setting lands in is an
// implementation detail nobody should have to remember, so this saves all of
// them and appears on every tab that can change something.
void DrawSaveBar() {
    if (ImGui::Button(SettingsDirty() ? "Save all settings *" : "Save all settings")) {
        SettingsSaveAll();
    }
    ImGui::SetItemTooltip("Writes gta2dx9_settings.ini, gta2dx9_lights.ini and "
                          "gta2dx9_effects.ini. All three are read back at startup.");
    ImGui::SameLine();
    if (ImGui::Button("Reload all")) {
        SettingsLoadAll();
        LightsLoadOverrides();
        SyntheticLightsLoad(nullptr);
        LightsInvalidate(0);
    }
    ImGui::SameLine();
    ImGui::TextColored(SettingsDirty() ? kWarn : kDim, "%s%s", SettingsPath(),
                       SettingsDirty() ? "  (unsaved)" : "");
}

void DrawStatus() {
    DrawSaveBar();
    ImGui::Separator();
    ImGui::TextColored(RemixApiAvailable() ? kGood : kBad, "Remix API: %s", RemixApiStatusText());

    const char* scene = LightsScene();
    ImGui::TextColored(scene[0] ? kGood : kDim, "District: %s",
                       scene[0] ? scene : "(not identified yet)");

    const bool gameLighting = game::LightingEnabled();
    ImGui::TextColored(gameLighting ? kGood : kBad, "Game lighting flag: %s",
                       gameLighting ? "on" : "OFF");
    if (!gameLighting) {
        ImGui::TextColored(kBad, "  GTA2 only builds its light list when this is set. Turn "
                                 "lighting back on in the GTA2 Manager / gta2.cfg -- with it "
                                 "off the game never calls gbh_AddLight and there is nothing "
                                 "to inject.");
    }

    ImGui::Text("Game ambient: %.3f", LightsAmbient());
    ImGui::SetItemTooltip("GTA2's additive brightness floor, from gbh_SetAmbient. At 1.0 the "
                          "original renderer skipped lighting altogether -- that is its "
                          "daylight fast path.");

    const RemixLightStats& st = LightsStats();
    ImGui::Separator();
    ImGui::Text("submitted by the game %d   tracked %d (%d moving)   drawn last frame %d",
                st.submittedLastFrame, st.tracked, st.moving, st.drawnLastFrame);
    ImGui::Text("suppressed: %d override, %d class filter, %d below the intensity floor",
                st.suppressedByOverride, st.suppressedByClass, st.suppressedByIntensity);
    if (st.tracked > 0) {
        ImGui::Text("game intensity in view: %.2f .. %.2f, %d of %d at full",
                    st.minIntensitySeen, st.maxIntensitySeen, st.atFullIntensity, st.tracked);
        ImGui::SetItemTooltip("Mostly 1.00 is expected, not a bug: 96%% of the map lights in the "
                              "shipped districts are at full intensity. Traffic lights and "
                              "vehicle lamps come in at 0.78, and a blinking light drops to 0 "
                              "while it is dark. The real variation is in reach and colour.");
    }
    ImGui::Text("created %u   redefined %u   destroyed %u", st.created, st.updated, st.destroyed);
    ImGui::TextColored(kDim, "light lists seen %u, %u frame(s) since the last one",
                       st.collectsSeen, st.framesSinceCollect);

    if (st.applyFailures) {
        ImGui::TextColored(kBad, "CreateLight failed %u time(s), last code %d (%s)",
                           st.applyFailures, st.lastApplyError, ErrorText(st.lastApplyError));
    }
    if (st.collectsSeen == 0 && gameLighting) {
        ImGui::TextColored(kWarn, "The game has not listed a single light yet. That is normal "
                                  "in the menus -- it only happens during a world render.");
    }
    if (st.tracked > 0 && st.drawnLastFrame == 0 && LightsSettings().enabled) {
        ImGui::TextColored(kBad, "Nothing is reaching Remix: lights are tracked but none drew.");
    }
}

void DrawTuning() {
    RemixLightSettings& s = LightsSettings();

    ImGui::Checkbox("Inject lights", &s.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Static", &s.injectStatic);
    ImGui::SetItemTooltip("Street lamps, neon, traffic lights -- everything from the map's LGHT "
                          "chunk that never moves.");
    ImGui::SameLine();
    ImGui::Checkbox("Moving", &s.injectMoving);
    ImGui::SetItemTooltip("Vehicle headlights and tail lights, train lamps, muzzle flashes. "
                          "Recognised by having been seen at more than one position.");

    ImGui::SeparatorText("Brightness");

    ImGui::SliderFloat("Brightness", &s.radianceScale, 0.1f, 1000.0f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("The master control, and the one to reach for. Radiance = colour x the "
                          "game's intensity x this x the reach term below.\n\n"
                          "GTA2 sets 96% of its map lights to intensity 1.0, so its own intensity "
                          "carries almost no variation -- this slider is where overall brightness "
                          "comes from, and the reach exponent is what spreads the lights apart.");

    ImGui::SliderFloat("Moving light brightness", &s.movingScale, 0.0f, 20.0f, "%.2f");
    ImGui::SetItemTooltip("Extra multiplier on headlights and flashes only. They are small and "
                          "brief and rarely want the same brightness as a street lamp.");

    ImGui::SeparatorText("Colour");

    ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f, "%.2f");
    ImGui::SetItemTooltip("GTA2's light palette is far more saturated than anything real -- pure "
                          "#FF8000 sodium, pure #00FFFF neon -- and a path tracer bouncing that "
                          "around exaggerates it further. Pulls each colour towards its own "
                          "luminance, so turning it down does not also darken the scene. "
                          "1.0 leaves the game's colours alone; 0 makes every light white.");

    ImGui::SeparatorText("Shape and reach");

    ImGui::SliderFloat("Emitter radius", &s.emitterRadius, 0.01f, 2.0f, "%.3f");
    ImGui::SetItemTooltip("Physical size of the light sphere, in map tiles. This is NOT the "
                          "game's radius -- that is the light's reach, and using it here would "
                          "put a glowing ball the width of the street around every lamp.");

    ImGui::SliderFloat("Reach exponent", &s.radiusExponent, 0.0f, 2.0f, "%.2f");
    ImGui::SetItemTooltip("How strongly the game's radius feeds brightness. 0 ignores it and "
                          "every light is equally bright; 1 is linear; 2 is closer to physically "
                          "consistent but very wide-ranging.");

    ImGui::SliderFloat("Reference reach", &s.referenceRadius, 0.5f, 8.0f, "%.2f");
    ImGui::SetItemTooltip("The reach that lands exactly on the radiance scale. 3 tiles is the "
                          "commonest radius in the shipped maps.");

    ImGui::SliderFloat("Intensity floor", &s.minIntensity, 0.0f, 0.2f, "%.4f");
    ImGui::SetItemTooltip("Lights dimmer than this are dropped rather than emitted. GTA2 blinks "
                          "a light by taking its intensity to zero, so without a floor a traffic "
                          "light glows red, amber and green at once.");

    ImGui::Separator();
    ImGui::Checkbox("Ambient fill light", &s.ambientFill);
    ImGui::SetItemTooltip("GTA2's ambient is a constant added to every lit vertex, which a path "
                          "tracer has no equivalent for. This stands a dim overhead distant "
                          "light in for it. Off by default: it is an approximation, not what the "
                          "game did.");
    if (s.ambientFill) {
        ImGui::SliderFloat("Ambient fill scale", &s.ambientFillScale, 0.0f, 20.0f, "%.2f");
    }

    ImGui::Separator();
    if (ImGui::Button(LightsOverridesDirty() ? "Save overrides *" : "Save overrides")) {
        LightsSaveOverrides();
    }
    ImGui::SetItemTooltip("Saving is manual on purpose: most of what happens in here is "
                          "experiment, and an autosave would make every stray drag permanent.");
    ImGui::SameLine();
    if (ImGui::Button("Reload overrides")) {
        LightsLoadOverrides();
        LightsInvalidate(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Redefine all")) LightsInvalidate(0);
    ImGui::SameLine();
    ImGui::TextColored(LightsOverridesDirty() ? kWarn : kDim, "%d override(s) in %s%s",
                       static_cast<int>(LightsOverrides().size()), LightsOverridePath(),
                       LightsOverridesDirty() ? "  (unsaved)" : "");
}

// The one control both light systems share: does this light belong to the night?
//
// Drawn as the two sun elevations rather than as clock times, because that is
// what actually decides it - the same lamp comes on later in June than in
// December and at a different hour at a different latitude, and reading it off
// the sun gets all of that for free.
bool DrawGate(DaylightGate& gate, const char* what) {
    bool changed = ImGui::Checkbox("Off during the day", &gate.enabled);
    ImGui::SetItemTooltip("GTA2 has no day of its own, so this is left to every light "
                          "individually rather than assumed.");
    if (gate.enabled) {
        ImGui::SameLine();
        const float now = DaylightGateFactor(gate);
        ImGui::TextColored(now > 0.99f ? kGood : (now < 0.01f ? kDim : kWarn), "  %s: %.0f%%",
                           what, now * 100.0f);

        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Out by (sun elevation)", &gate.offAboveDeg, -20.0f, 20.0f,
                                      "%.1f deg");
        ImGui::SetItemTooltip("Fully off once the sun is this high. 0 is the horizon.");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Full on by", &gate.onBelowDeg, -30.0f, 10.0f, "%.1f deg");
        ImGui::SetItemTooltip("Fully on once the sun is this far down. -6 is the end of civil "
                              "twilight, which is when real street lighting is at full.");
        if (gate.offAboveDeg <= gate.onBelowDeg) {
            ImGui::TextColored(kWarn, "  These are the wrong way round, so it switches hard "
                                      "instead of fading.");
        }
    }
    return changed;
}

void DrawCategory(int category) {
    SyntheticCategorySettings& c = SyntheticLightsSettings().category[category];
    ImGui::PushID(category);

    bool changed = ImGui::Checkbox("Enabled", &c.enabled);
    ImGui::SameLine();
    ImGui::ColorButton("##swatch", ImVec4(c.rgb[0], c.rgb[1], c.rgb[2], 1.0f),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
    ImGui::SameLine();
    changed |= ImGui::ColorEdit3("Colour", c.rgb, ImGuiColorEditFlags_NoInputs);

    changed |= ImGui::SliderFloat("Intensity", &c.intensity, 0.0f, 8.0f, "%.3f",
                                  ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Multiplied by the global brightness on the Status tab, like every "
                          "other light.");
    changed |= ImGui::SliderFloat("Reach (tiles)", &c.radius, 0.1f, 12.0f, "%.2f");
    changed |= ImGui::SliderFloat("Height offset", &c.heightOffset, -0.5f, 2.0f, "%.3f");
    changed |= ImGui::SliderInt("Max lights", &c.maxLights, 1, 128);
    ImGui::SetItemTooltip("Every light is a CreateLight across the 32-bit Remix bridge, and some "
                          "particle types arrive in bursts of dozens. This is the ceiling per "
                          "frame for this category.");

    ImGui::SeparatorText("Time of day");
    changed |= DrawGate(c.gate, "burning");

    if (category == kSynthHeadlight) {
        ImGui::SeparatorText("Beam");
        changed |= ImGui::SliderFloat("Cone angle", &c.coneAngleDeg, 5.0f, 90.0f, "%.1f deg");
        changed |= ImGui::SliderFloat("Downward pitch", &c.pitchDegrees, 0.0f, 45.0f, "%.1f deg");
        changed |= ImGui::SliderFloat("Forward from centre", &c.forwardOffset, 0.0f, 2.0f, "%.3f");
        ImGui::SetItemTooltip("Distance from the car's centre to its nose, in tiles.");
        changed |= ImGui::SliderFloat("Beam separation", &c.sideOffset, 0.0f, 1.0f, "%.3f");
        ImGui::SetItemTooltip("Half the spacing between the two beams.");
        changed |= ImGui::SliderFloat("Cone softness", &LightsSettings().coneSoftness, 0.0f, 1.0f,
                                      "%.2f");

        ImGui::SeparatorText("Per car model");
        if (ImGui::Checkbox("Use per-model beams", &SyntheticLightsSettings().perModelBeams)) {
            SyntheticLightsMarkDirty();
        }
        ImGui::TextWrapped(
            "Off by default, and worth being blunt about why: nothing here reads the car's actual "
            "width. The model id is available, so a value can be stored per model, but it is a "
            "number you typed rather than anything measured - which is why one setting for every "
            "car is the honest default until the width is really read from the game. Overrides "
            "below apply only while this is on; 0 on a field means use the value above.");
        if (ImGui::BeginTable("beams", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                  | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 220.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("cone", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("separation", ImGuiTableColumnFlags_WidthFixed, 150.0f);
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
                if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.75f, 1.0f));
                if (ImGui::SmallButton(lit ? "lit" : "light")) {
                    SyntheticHighlightModel(lit ? -1 : m.model);
                }
                if (lit) ImGui::PopStyleColor();

                BeamOverride* b = SyntheticFindBeam(m.model);
                BeamOverride edit = b ? *b : BeamOverride();
                bool touched = false;
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                touched |= ImGui::SliderFloat("##cone", &edit.coneAngleDeg, 0.0f, 90.0f, "%.0f deg");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                touched |= ImGui::SliderFloat("##side", &edit.sideOffset, 0.0f, 1.0f, "%.3f");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                touched |= ImGui::SliderFloat("##fwd", &edit.forwardOffset, 0.0f, 2.0f, "%.3f");
                if (touched) SyntheticEditBeam(m.model) = edit;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (changed) SyntheticLightsMarkDirty();
    ImGui::PopID();
}

void DrawSynthetic() {
    SyntheticSettings& s = SyntheticLightsSettings();
    const SyntheticStats& st = SyntheticLightsStats();

    ImGui::TextWrapped(
        "GTA2 emits no light for gunfire, bullets, sparks or cigarettes, and its headlamps are "
        "point lights with no beam. These are invented from the game's live particle and vehicle "
        "lists. Which particle type is which effect is not written down anywhere in the game, so "
        "it is bound on the Effects tab by triggering the effect and watching which id appears.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Inject invented lights", &s.enabled)) SyntheticLightsMarkDirty();
    ImGui::SameLine();
    ImGui::TextColored(kDim, "particles walked %d  |  vehicles %d, %d driven", st.particlesWalked,
                       st.vehiclesWalked, st.vehiclesDriven);

    if (!st.particleListFound) {
        ImGui::TextColored(kWarn, "No particle list -- normal in the menus, and while nothing has "
                                  "spawned one yet.");
    }
    if (!st.vehicleListFound) {
        ImGui::TextColored(kWarn, "No vehicle list yet.");
    }

    ImGui::Spacing();
    if (ImGui::BeginTabBar("categories")) {
        for (int i = 0; i < kSynthCategoryCount; ++i) {
            if (!ImGui::BeginTabItem(SyntheticCategoryName(i))) continue;

            int boundTypes = 0;
            for (int t = 0; t < kMaxParticleType; ++t) {
                if (SyntheticTypeBinding(t) == i) ++boundTypes;
            }
            if (i == kSynthHeadlight) {
                ImGui::TextColored(kDim, "From the vehicle list. A car is driven when someone is "
                                         "sitting in it -- vehicle+0x54, a pointer on every "
                                         "moving car and on no parked one.");
                bool changed = false;
                changed |= ImGui::Checkbox("Needs a driver", &s.drivenNeedsDriver);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("...or just moving", &s.drivenAllowsMovement);
                ImGui::SetItemTooltip("Fallback for a car being pushed. Off by default: the "
                                      "occupant test already covers a car stopped at a red "
                                      "light, which is what the movement window was for.");
                if (s.drivenAllowsMovement) {
                    int window = static_cast<int>(s.drivenWindowMs);
                    changed |= ImGui::SliderInt("Movement window (ms)", &window, 0, 10000);
                    s.drivenWindowMs = static_cast<unsigned>(window);
                    changed |= ImGui::SliderFloat("Movement threshold", &s.drivenMinMovement, 0.0f,
                                                  0.05f, "%.4f");
                }
                if (changed) SyntheticLightsMarkDirty();
            } else if (boundTypes == 0) {
                ImGui::TextColored(kBad, "No particle type is bound to this category, so it emits "
                                         "nothing. Go to the Effects tab, trigger the effect in "
                                         "game, and bind the id that appears.");
            } else {
                ImGui::TextColored(kGood, "%d particle type(s) bound.", boundTypes);
            }
            ImGui::Text("emitted last frame: %d   (%u since start)", st.emitted[i],
                        st.totalEmitted[i]);
            ImGui::SetItemTooltip("The running total is the one to watch for anything brief: a "
                                  "spark burst lives three frames, so the per-frame count reads "
                                  "zero almost every time you look at it.");
            if (st.capped[i]) {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, "(%d dropped at the ceiling)", st.capped[i]);
            }
            ImGui::Separator();
            DrawCategory(i);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button(SyntheticLightsDirty() ? "Save effect settings *" : "Save effect settings")) {
        SyntheticLightsSave();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) SyntheticLightsLoad(nullptr);
    ImGui::SameLine();
    ImGui::TextColored(SyntheticLightsDirty() ? kWarn : kDim, "%s%s", SyntheticLightsPath(),
                       SyntheticLightsDirty() ? "  (unsaved)" : "");
}

void DrawEffectInspector() {
    ImGui::TextWrapped(
        "Every particle type the game has spawned since this list was last cleared. GTA2 tags each "
        "particle with a type id but nothing names them, so this is how a category gets bound: "
        "clear the list, trigger one effect -- fire a gun, scrape a wall, stand still until the "
        "cigarette comes out -- and the id that appears is the one. Rows seen in the last two "
        "seconds are highlighted.\n\n"
        "The behaviour columns are what actually tell the effects apart, and they are how bullet, "
        "sparks and fire were identified: a bullet is far faster than anything else and travels "
        "alone; sparks arrive two dozen at once and are gone in three frames; a fire lasts ten "
        "times longer than that and clusters. Speed is tiles per frame, life is frames.");

    ImGui::Spacing();
    if (ImGui::Button("Clear list")) SyntheticForgetParticleTypes();
    ImGui::SameLine();
    if (ImGui::Button("Run structure probe")) SyntheticProbe("F4 menu");
    ImGui::SetItemTooltip("Writes an offset report to gta2dx9.log: which field in a particle or a "
                          "vehicle is the position, and which is the driver. Scored against the "
                          "camera position and against which cars are moving, so it does not "
                          "depend on anything having been read correctly off a decompiler.");
    ImGui::SameLine();
    const SyntheticStats& st = SyntheticLightsStats();
    ImGui::TextColored(kDim, "%d type(s) seen, %d particle(s) live",
                       static_cast<int>(SyntheticParticleTypes().size()), st.particlesWalked);

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("types", 9, flags, ImVec2(0.0f, 380.0f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("spawns", ImGuiTableColumnFlags_WidthFixed, 58.0f);
    ImGui::TableSetupColumn("burst", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("speed", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("rise", ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("life", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("height", ImGuiTableColumnFlags_WidthFixed, 54.0f);
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Paint every particle of this type bright magenta, whatever it is "
                              "bound to. Look at the screen: that is the only way to tell the "
                              "fire from the smoke above it from the litter on the pavement.");
        }
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

    ImGui::Separator();
    ImGui::TextColored(kDim, "Light settings for each category, the same controls as the Invented "
                             "tab - here too because this is where you are standing when you "
                             "decide a fire is too dim.");
    if (ImGui::BeginTabBar("effectcats")) {
        for (int i = 0; i < kSynthCategoryCount; ++i) {
            if (!ImGui::BeginTabItem(SyntheticCategoryName(i))) continue;
            const SyntheticStats& s = SyntheticLightsStats();
            ImGui::Text("emitted last frame: %d   (%u since start)", s.emitted[i],
                        s.totalEmitted[i]);
            DrawCategory(i);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void HourText(float hour, char* out, size_t size) {
    int h = static_cast<int>(hour);
    int m = static_cast<int>((hour - h) * 60.0f + 0.5f);
    if (m >= 60) { m -= 60; ++h; }
    if (h >= 24) h -= 24;
    _snprintf(out, size - 1, "%02d:%02d", h, m);
    out[size - 1] = '\0';
}

// What the sky is doing, and every knob that decides it.
void DrawTimeOfDay() {
    DrawSaveBar();
    ImGui::Separator();

    TimeOfDaySettings& tod = TimeOfDay();

    ImGui::TextWrapped(
        "Remix places its sun from two config variables, both in degrees and both documented in "
        "the runtime as game-drivable per frame: rtx.atmosphere.sunElevation and "
        "rtx.atmosphere.sunRotation. They are pushed through the Remix API's SetConfigVariable, "
        "which is the supported way for a game to move the sky.\n\n"
        "The angles are not invented -- they come from the standard solar position equations for "
        "a latitude and a season, so the sun rises in the east, is highest at local noon, sets in "
        "the west, and spends the night as far below the horizon as it really would. That last "
        "part is what makes 2 am dark rather than merely dim.");

    ImGui::Separator();
    if (!RemixApiAvailable()) {
        ImGui::TextColored(kBad, "Remix API not available: %s", RemixApiStatusText());
        ImGui::TextColored(kDim, "The clock below still runs, and everything can be set up; "
                                 "nothing reaches the sky until Remix answers.");
    } else {
        ImGui::TextColored(TimeOfDayPushed() ? kGood : kWarn, "%s", TimeOfDayStatus());
        ImGui::SameLine();
        ImGui::TextColored(kDim, "(%d push(es))", TimeOfDayPushCount());
    }

    if (ImGui::Checkbox("Run the day/night cycle", &tod.enabled)) SettingsMarkDirty();
    ImGui::SetItemTooltip("Off leaves whatever sun rtx.conf last set, which is how it behaved "
                          "before this existed.");

    ImGui::SeparatorText("Clock");

    char clock[16];
    HourText(TimeOfDayHour(), clock, sizeof(clock));
    float elevation = 0.0f, rotation = 0.0f;
    TimeOfDayAngles(&elevation, &rotation);

    const bool daylight = elevation > 0.0f;
    ImGui::TextColored(daylight ? kWarn : kDim, "%s", clock);
    ImGui::SameLine();
    ImGui::Text("   sun elevation %+.2f deg, rotation %.2f deg", elevation, rotation);
    ImGui::SameLine();
    // The civil/nautical/astronomical twilight boundaries, which is the honest
    // way to say how dark it is rather than "night".
    const char* phase = elevation > 0.0f      ? "day"
                        : elevation > -6.0f   ? "civil twilight"
                        : elevation > -12.0f  ? "nautical twilight"
                        : elevation > -18.0f  ? "astronomical twilight"
                                              : "night";
    ImGui::TextColored(kDim, "  %s", phase);

    float hour = TimeOfDayHour();
    if (ImGui::SliderFloat("Time", &hour, 0.0f, 24.0f, clock)) TimeOfDaySetHour(hour);
    ImGui::SetItemTooltip("Drag to scrub the day. The clock carries on from wherever it is let "
                          "go, unless it is paused.");

    if (ImGui::Checkbox("Pause the clock", &tod.paused)) SettingsMarkDirty();
    ImGui::SameLine();
    if (ImGui::Button("Back to the start hour")) TimeOfDayReset();
    ImGui::SameLine();
    if (ImGui::Button("Noon")) TimeOfDaySetHour(12.0f);
    ImGui::SameLine();
    if (ImGui::Button("Midnight")) TimeOfDaySetHour(0.0f);

    ImGui::Spacing();
    float startHour = tod.startHour;
    char startText[16];
    HourText(startHour, startText, sizeof(startText));
    if (ImGui::SliderFloat("Start of a level", &startHour, 0.0f, 24.0f, startText)) {
        tod.startHour = startHour;
        SettingsMarkDirty();
    }
    ImGui::SetItemTooltip("Where the clock is set when a level begins. 02:00 is the default and "
                          "is deep night at every latitude and season below.");

    if (ImGui::SliderFloat("Game minutes per real second", &tod.minutesPerSecond, 0.1f, 60.0f,
                           "%.2f", ImGuiSliderFlags_Logarithmic)) {
        SettingsMarkDirty();
    }
    ImGui::SetItemTooltip("1.00 is one real second to one game minute, which is a full day in 24 "
                          "real minutes. That is the default.");
    ImGui::TextColored(kDim, "A full day takes %.1f real minutes at this rate.",
                       tod.minutesPerSecond > 0.0f ? 1440.0f / tod.minutesPerSecond / 60.0f
                                                   : 0.0f);

    ImGui::SeparatorText("Where and when on Earth");
    ImGui::TextWrapped(
        "Anywhere City is nowhere in particular, so this is a choice rather than a fact. Latitude "
        "sets how high the sun climbs and how steeply it rises and sets; declination sets the "
        "season -- +23.4 at the June solstice, 0 at either equinox, -23.4 in December. The "
        "defaults are a temperate northern latitude at the equinox: twelve hours of daylight, a "
        "sun that reaches 50 degrees at noon, long slanted shadows morning and evening.");

    bool changed = false;
    changed |= ImGui::SliderFloat("Latitude (deg)", &tod.latitudeDeg, -66.0f, 66.0f, "%.1f");
    ImGui::SetItemTooltip("Positive is north. Past about 66 the sun stops rising or setting at "
                          "the solstices, which is a fine thing to look at and a poor thing to "
                          "play in.");
    changed |= ImGui::SliderFloat("Declination (deg)", &tod.declinationDeg, -23.44f, 23.44f,
                                  "%.2f");
    ImGui::SetItemTooltip("The season. +23.44 = midsummer in the north, 0 = equinox, "
                          "-23.44 = midwinter.");

    struct Season { float declination; const char* label; };
    static const Season seasons[] = {
        {23.44f, "June solstice"},
        {0.0f, "Equinox"},
        {-23.44f, "December solstice"},
    };
    for (const Season& s : seasons) {
        ImGui::PushID(&s);
        if (ImGui::SmallButton(s.label)) {
            tod.declinationDeg = s.declination;
            changed = true;
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::SeparatorText("Fitting it to the city");
    ImGui::TextWrapped(
        "Remix's own reference for sunRotation is not documented, and GTA2's streets do not have "
        "to run north-south anyway, so the compass bearing can be turned and, if it comes out "
        "mirrored, flipped. The elevation offset is a thumb on the scale for pulling the night up "
        "out of pitch black.");
    changed |= ImGui::SliderFloat("Rotation offset (deg)", &tod.rotationOffsetDeg, -180.0f, 180.0f,
                                  "%.1f");
    changed |= ImGui::Checkbox("Sun travels clockwise seen from above", &tod.rotationClockwise);
    ImGui::SetItemTooltip("Which it does in the northern hemisphere. Uncheck if the sun ends up "
                          "rising where it should set.");
    changed |= ImGui::SliderFloat("Elevation offset (deg)", &tod.elevationOffsetDeg, -30.0f, 30.0f,
                                  "%.1f");
    changed |= ImGui::SliderFloat("Push interval (ms)", &tod.pushIntervalMs, 0.0f, 500.0f, "%.0f");
    ImGui::SetItemTooltip("Each push is a round trip across the 32-bit Remix bridge and the sun "
                          "moves by a fraction of a degree per frame, so there is nothing to gain "
                          "from doing it every frame. 50 ms is twenty a second.");
    if (changed) SettingsMarkDirty();

    ImGui::SeparatorText("What the clock is switching");
    ImGui::TextWrapped(
        "GTA2 has no day of its own, so nothing in it knows to go out at dawn. Each kind of light "
        "decides for itself, from the sun's elevation rather than from the clock -- the full "
        "controls are on the Lights tab for the game's own lights and the Invented tab for ours.");
    if (ImGui::BeginTable("gates", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("light");
        ImGui::TableSetupColumn("follows the sun");
        ImGui::TableSetupColumn("burning now");
        ImGui::TableHeadersRow();
        auto row = [](const char* name, bool gated, float factor) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::TextColored(gated ? kGood : kDim, "%s", gated ? "yes" : "always on");
            ImGui::TableNextColumn();
            ImGui::TextColored(factor > 0.99f ? kGood : (factor < 0.01f ? kDim : kWarn), "%.0f%%",
                               factor * 100.0f);
        };
        const RemixLightSettings& ls = LightsSettings();
        for (int i = 0; i < kGameLightClassCount; ++i) {
            char label[48];
            _snprintf(label, sizeof(label) - 1, "%s (game)", GameLightClassName(i));
            label[sizeof(label) - 1] = 0;
            row(label, ls.gameClass[i].gate.enabled,
                DaylightGateFactor(ls.gameClass[i].gate));
        }
        for (int i = 0; i < kSynthCategoryCount; ++i) {
            char label[48];
            _snprintf(label, sizeof(label) - 1, "%s (invented)", SyntheticCategoryName(i));
            label[sizeof(label) - 1] = 0;
            row(label, SyntheticLightsSettings().category[i].gate.enabled,
                SyntheticCategoryDaylight(i));
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("The day, hour by hour");
    if (ImGui::BeginTable("tod", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("time");
        ImGui::TableSetupColumn("elevation");
        ImGui::TableSetupColumn("rotation");
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
            ImGui::Text("%6.2f", r);
            ImGui::TableNextColumn();
            // A bar, so the shape of the day is visible without a plot.
            const int filled = static_cast<int>((e + 90.0f) / 180.0f * 40.0f + 0.5f);
            char bar[48];
            for (int i = 0; i < 40; ++i) bar[i] = i < filled ? '#' : '.';
            bar[40] = '\0';
            ImGui::TextColored(e > 0.0f ? kWarn : kDim, "%s", bar);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kDim, "The middle of the bar is the horizon. Times are the current "
                             "latitude and season, not the current clock.");
}

void DrawFrameRate() {
    ImGui::SeparatorText("Frame rate");
    ImGui::TextWrapped(
        "GTA2's own cap is a checkbox, not a number. Its pacer waits on a hardcoded 33 ms step "
        "(gta2.exe!0x0045A460 returns 33) and the two registry values the manager writes, "
        "max_frame_rate and min_frame_rate, are read as plain booleans -- capped means 30 fps, "
        "uncapped means it stops waiting altogether. So the number is ours: the game's cap is "
        "left off and the frame is held here instead.\n\n"
        "What it costs, plainly: GTA2 advances its simulation exactly one step per rendered "
        "frame and scales nothing by elapsed time, which is why uncapped runs fast rather than "
        "merely smooth. A cap of N runs the game at N/30.3 times its intended speed. There is no "
        "setting that gives 60 fps of motion at 1x speed -- that would need the game's per-step "
        "constants halved, which a renderer cannot do.");

    float fps = FrameLimitFps();
    if (ImGui::SliderFloat("Frame cap", &fps, 0.0f, 240.0f, fps <= 0.0f ? "off" : "%.0f fps")) {
        FrameLimitSet(fps);
        SettingsMarkDirty();
    }
    ImGui::SetItemTooltip("0 turns the cap off and lets the game free-run, which is as fast as "
                          "the machine allows and correspondingly frantic.");

    struct Preset { float fps; const char* label; const char* note; };
    static const Preset presets[] = {
        {0.0f,      "off",   "free-running; as fast as the machine goes"},
        {30.3030f,  "30.30", "1.00x -- exactly GTA2's own 33 ms step. The default."},
        {45.0f,     "45",    "1.49x"},
        {60.0f,     "60",    "1.98x -- smooth, and visibly brisk"},
        {90.0f,     "90",    "2.97x"},
    };
    for (const Preset& p : presets) {
        ImGui::PushID(&p);
        if (ImGui::SmallButton(p.label)) {
            FrameLimitSet(p.fps);
            SettingsMarkDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(kDim, "%s", p.note);
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Text("measured %.1f fps", FrameLimitMeasuredFps());
    ImGui::SameLine();
    const float idle = FrameLimitIdleFraction();
    if (FrameLimitFps() > 0.0f && idle < 0.02f) {
        ImGui::TextColored(kWarn, "  (0%% of the frame spent waiting -- the machine, not the cap, "
                                  "is what is limiting this)");
    } else {
        ImGui::TextColored(kDim, "  %.0f%% of each frame spent waiting on the cap", idle * 100.0f);
    }
    ImGui::TextColored(kDim, "Persist a value with fps_cap under [renderer] in gta2dx9.ini, or "
                             "just save the settings.");
}

// Fire, explosions and muzzle flashes: the sprites whose black rim is in the
// artwork rather than behind the alpha. See texture_store.h.
void DrawEffectSprites() {
    EffectSpriteSettings& fx = EffectSprites();

    ImGui::SeparatorText("Fire, explosions and flashes");
    ImGui::TextWrapped(
        "These sprites had a black border that no amount of alpha work would shift, and the "
        "reason is that the black is not behind the alpha at all: the artwork fades to black on "
        "its way out to the cutout edge, and those texels are fully opaque. An explosion frame "
        "reads (8,0,0) at alpha 255 a texel in from the hole. Art drawn that way only makes sense "
        "additively, where black adds nothing -- but GTA2's renderers never blended: d3ddll.dll "
        "sets no blend state at all and the 3dfx one draws colour-keyed, so on a CRT the rim was "
        "simply lived with. Under a path tracer an opaque black surface is as black as black "
        "gets.\n\n"
        "Which sprites those are is read off the artwork, not off a list: a black outline is a "
        "thing no ordinary cutout sprite has.\n\n"
        "It deliberately does not also require a bright core. It used to, and that was wrong: an "
        "explosion's last frames are embers and smoke, dark all over, and the core test threw "
        "exactly those out -- so the animation went from a glow to an opaque black blob on its "
        "final frame, which is worse than never having fixed it. A dying fire is still a fire, "
        "and additively it fades to nothing, which is what it should do.");

    int mode = fx.mode == EffectSpriteMode::Additive ? 2
                                                     : (fx.mode == EffectSpriteMode::Cutout ? 1 : 0);
    const int was = mode;
    ImGui::RadioButton("Leave them alone", &mode, 0);
    ImGui::SetItemTooltip("The old behaviour, black rim included.");
    ImGui::SameLine();
    ImGui::RadioButton("Fade the fringe out", &mode, 1);
    ImGui::SetItemTooltip("Turns the artwork's own brightness into alpha and keeps the alpha "
                          "test, so the dark edge is discarded rather than painted. No glow, but "
                          "the geometry stays opaque to the path tracer.");
    ImGui::SameLine();
    ImGui::RadioButton("Draw them additively", &mode, 2);
    ImGui::SetItemTooltip("What the artwork was drawn for. Remix treats additive draws as "
                          "emissive particles, so a fireball glows over the street instead of "
                          "sitting on it as a flat decal.");
    if (mode != was) {
        fx.mode = mode == 2 ? EffectSpriteMode::Additive
                            : (mode == 1 ? EffectSpriteMode::Cutout : EffectSpriteMode::Off);
        RebuildDeviceTextures();
        SettingsMarkDirty();
    }

    ImGui::Spacing();
    ImGui::Text("%d of %d built frames classified as effects; %d sprite(s) drawn additively last "
                "frame", EffectSpriteFrames(), ClassifiedFrames(), EffectBatchesDrawn());
    if (fx.mode != EffectSpriteMode::Off && EffectSpriteFrames() == 0) {
        ImGui::TextColored(kDim, "Nothing yet -- blow something up, or loosen the thresholds "
                                 "below.");
    }

    ImGui::Spacing();
    bool changed = false;
    changed |= ImGui::SliderFloat("Fringe darker than", &fx.edgeLuma, 0.0f, 64.0f, "%.0f");
    ImGui::SetItemTooltip("Median luminance of the opaque texels touching the hole. Measured over "
                          "a capture of the shipped districts, fire and explosion frames sit at "
                          "about 5; the darkest thing that is not an effect -- a pedestrian in "
                          "shadow -- sits at 23.");
    changed |= ImGui::SliderFloat("...or all of it darker than", &fx.faintLuma, 0.0f, 128.0f,
                                  "%.0f");
    ImGui::SetItemTooltip("The second route in: the brightest texel anywhere in the sprite. An "
                          "explosion's last frames are embers and smoke, dark all over and with "
                          "no crisp outline left -- and an opaque near-black world sprite is "
                          "wrong under a path tracer whatever it is. Nothing in the capture that "
                          "is not an effect comes close; the darkest pedestrian peaks at 105. "
                          "0 turns this route off.");
    if (fx.mode == EffectSpriteMode::Cutout) {
        changed |= ImGui::SliderFloat("Fade gain", &fx.cutoutGain, 0.5f, 8.0f, "%.2f");
        ImGui::SetItemTooltip("alpha = luminance x this. Higher keeps more of the fireball's "
                              "dimmer edge.");
    }
    if (changed) SettingsMarkDirty();

    ImGui::Spacing();
    if (ImGui::Button("Rebuild textures now")) RebuildDeviceTextures();
    ImGui::SameLine();
    ImGui::TextColored(kDim, "The classification happens when a texture is built, so a threshold "
                             "change only reaches artwork that is rebuilt.");

    ImGui::SeparatorText("Sprite geometry");
    ImGui::TextWrapped(
        "Each sprite is one draw call of a fixed quad in its own object space, with the placement "
        "in the world matrix. That is what lets RTX Remix recognise a sprite as the same one it "
        "saw last frame and work out a motion vector for it; batching them into shared "
        "world-space vertex buffers gave it geometry whose vertex count and vertex order changed "
        "every frame, so it either treated a moving sprite as static or paired one sprite's "
        "vertices with another's. See the note at the top of live_geometry.h.\n\n"
        "The count below is the health check: it should climb for a few seconds after a level "
        "loads and then sit still. If it keeps climbing, some sprite's object-space quad is "
        "dithering and its motion vectors will be no better than before.");
    int shapes = 0, shapeTextures = 0;
    SpriteShapeCounts(&shapes, &shapeTextures);
    ImGui::Text("%d distinct object-space quad(s) across %d sprite texture(s)", shapes,
                shapeTextures);

    ImGui::SeparatorText("Texture report");
    ImGui::TextWrapped(
        "A threshold is only as good as the cases it was set from, so every distinct frame this "
        "session builds keeps the numbers it was judged on, and the sprite pass records which "
        "frames it actually drew -- that is what separates a fireball from a road tile that "
        "happens to have a hole in it. A sprite that still has a black rim is then a row to look "
        "up rather than a guess.\n\n"
        "Written on exit as gta2dx9_textures.csv beside the game, with the frames themselves as "
        "TGAs alongside. Trigger the sprites, quit, and both are waiting.");

    bool dumping = TextureDumping();
    if (ImGui::Checkbox("Dump each frame as a TGA", &dumping)) SetTextureDumping(dumping);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%d written to %s", TextureFramesDumped(), TextureDumpDir());

    if (ImGui::Button("Write the report now")) {
        const int rows = WriteTextureReport();
        Log("texture report: written from the menu, %d row(s)", rows);
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "It is also written automatically when the renderer shuts down.");
}

void DrawAlpha() {
    ImGui::SeparatorText("Cutout edges");
    ImGui::TextWrapped(
        "GTA2's artwork is palettised with entry 0 as a colour key, so there is no real alpha to "
        "blend - every edge is binary. The black rim that used to show was not the alpha but the "
        "colour behind it: keyed texels were transparent *black*, and every filter, above all the "
        "mipmaps Remix builds, averaged that black into the neighbouring colour. Those texels now "
        "carry their neighbours' colour instead, alpha untouched.\n\n"
        "Alpha test is still the default. A path tracer wants a cutout to be a cutout - a blended "
        "surface has no single depth for a ray to hit - but now that the colour is right the two "
        "are worth comparing by eye.");

    int mode = gta2::GetAlphaMode() == gta2::AlphaMode::Blend ? 1 : 0;
    if (ImGui::RadioButton("Alpha test", &mode, 0)) gta2::SetAlphaMode(gta2::AlphaMode::Test);
    ImGui::SameLine();
    if (ImGui::RadioButton("Alpha blend", &mode, 1)) gta2::SetAlphaMode(gta2::AlphaMode::Blend);

    ImGui::SeparatorText("Stacked sprites");
    ImGui::TextWrapped(
        "A GTA2 car is not one sprite: the body is drawn, then its lights, then any logo, each a "
        "separate quad at exactly the same height. The game never depth tested so it simply "
        "painted them in order; as real geometry they are coplanar and the depth test picks a "
        "winner per pixel, which is the flickering and the dropouts. Each sprite landing where "
        "another already is this frame goes one step higher than the last, in the order the game "
        "drew them, so the painter's order becomes a real stacking order.");
    float step = SpriteStackStep();
    if (ImGui::SliderFloat("Stack step (blocks)", &step, 0.0f, 0.05f, "%.4f")) {
        SetSpriteStackStep(step);
    }
    ImGui::SetItemTooltip("0 puts them all back on one plane. The default hundredth of a block is "
                          "invisible at this camera and well clear of the depth buffer's "
                          "resolving power.");

    ImGui::SeparatorText("Soft edges for sprites");
    ImGui::TextWrapped(
        "The alpha itself is 1-bit - GTA2's palette has one key colour and no gradient anywhere - "
        "so bleeding fixed the colour and the edge stayed a stencil. This invents the ramp the "
        "artwork never had, dimming each visible texel by how much of its neighbourhood is empty. "
        "A fence or a tree wants its hard edge, an explosion does not, so it applies to sprites "
        "only and starts at 0. Takes effect on textures built after the change; drive somewhere "
        "new or reload the level to rebuild the ones already cached.");
    float feather = SpriteFeather();
    if (ImGui::SliderFloat("Sprite edge softness", &feather, 0.0f, 1.0f, "%.2f")) {
        SetSpriteFeather(feather);
    }

    ImGui::SeparatorText("Alpha mode");
    int ref = gta2::GetAlphaRef();
    if (ImGui::SliderInt("Alpha test cutoff", &ref, 1, 254)) gta2::SetAlphaRef(ref);
    ImGui::SetItemTooltip("Where the binary cutoff falls. Only used in alpha test mode; blending "
                          "drops it to 1, which still discards the fully transparent texels so "
                          "they do not write depth and hide what is behind them.");
}

void DrawSprites() {
    DrawEffectSprites();
    ImGui::Separator();
    DrawAlpha();
    ImGui::Separator();
    float lift = SpriteLift();

    ImGui::TextWrapped(
        "GTA2 puts all four corners of a sprite's quad at the single level of the object "
        "underneath, and its own renderers never depth tested, so a flat quad lying exactly in "
        "the floor was fine. As real 3D geometry it is not: on a ramp the quad cuts straight "
        "through the lid and the uphill half of the sprite vanishes into it. This lifts the whole "
        "quad along its normal. It takes effect on the next frame -- sprites are rebuilt from the "
        "game's stream every frame.");

    ImGui::Spacing();
    if (ImGui::SliderFloat("Sprite lift (blocks)", &lift, 0.0f, 0.5f, "%.3f")) {
        SetSpriteLift(lift);
    }
    ImGui::SetItemTooltip("One block is one map tile, roughly 2 m at GTA2's scale.");

    ImGui::Spacing();
    ImGui::TextColored(kDim, "Measured over the shipped districts, on surfaces a sprite can "
                             "actually stand on (road or pavement):");
    if (ImGui::BeginTable("lifts", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("lift");
        ImGui::TableSetupColumn("clears, pedestrian");
        ImGui::TableSetupColumn("clears, car");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        struct Row { float value; const char* ped; const char* car; const char* note; };
        static const Row rows[] = {
            {1.0f / 256.0f, "0%",     "0%",   "breaks coplanarity only; flat ground"},
            {0.0625f,       "78%",    "0%",   "half the default"},
            {0.125f,        "99.5%",  "78%",  "default -- every 7 degree ramp"},
            {0.25f,         "100%",   "78%",  "buys nothing more for cars"},
            {0.5f,          "100%",   "99.5%","covers 26 degree ramps, floats everything"},
        };
        for (const Row& r : rows) {
            ImGui::PushID(&r);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[24];
            _snprintf(label, sizeof(label) - 1, "%.4f", r.value);
            label[sizeof(label) - 1] = '\0';
            if (ImGui::SmallButton(label)) SetSpriteLift(r.value);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.ped);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.car);
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", r.note);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kDim, "72%% of those surfaces are flat, 21.9%% are 7 degree ramps, 5.9%% "
                             "are 26 degree, 0.14%% are 45 degree.");

    ImGui::Spacing();
    if (ImGui::Button("Reset to default")) SetSpriteLift(kDefaultSpriteLift);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "Persist a value with sprite_lift_thousandths under [renderer] in "
                             "gta2dx9.ini (%d = the current setting).",
                       static_cast<int>(SpriteLift() * 1000.0f + 0.5f));

    ImGui::Spacing();
    ImGui::TextWrapped("A single lift is a compromise, not a fix: the quad stays horizontal while "
                       "the ground under it is not. Tilting each sprite to the lid beneath it "
                       "would remove the trade entirely, and needs the ground plane per corner.");
}

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

// The game's own lights, sorted into kinds, so the clock can put the ones that
// belong to the night out during the day.
void DrawMapLightClasses() {
    RemixLightSettings& s = LightsSettings();
    const RemixLightStats& st = LightsStats();

    ImGui::SeparatorText("The game's own lights, by kind");
    ImGui::TextWrapped(
        "GTA2's lights arrive with no type on them: the LGHT chunk is sixteen bytes of colour, "
        "position, radius, intensity and blink timing, and byte 13 -- which the published format "
        "docs call a shape field -- is really the blink jitter. So there is nothing to read.\n\n"
        "They are still separable, because what GTA2 puts in those fields is not arbitrary. Two "
        "populations do not come from the map at all: traffic signals are built at runtime at a "
        "hard-coded intensity of 200 with a colour that cycles red, amber and green, and vehicle "
        "lamps are hung on every car at spawn and move with it. The rest split by hue -- across "
        "the shipped maps 12083 lights carry 765 colours, and the common ones are unmistakable: "
        "#FF8000 and #FF8040 sodium, #62CC8C and #00FFFF neon, #FFFFFF and #FFDB5E white. It is a "
        "heuristic, and it is drawn from what is actually in the maps rather than guessed.\n\n"
        "GTA2 has no day, so every one of these burns at noon. The defaults have everything but "
        "the junction signals following the sun; a red light has to be legible in daylight.");

    if (ImGui::BeginTable("mapclasses", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("in view");
        ImGui::TableSetupColumn("lit now");
        ImGui::TableSetupColumn("");
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
    ImGui::TextColored(kDim, "\"in view\" is what the game submitted this frame, before any of "
                             "this; \"lit now\" is what survived it.");
}

void DrawLightTable() {
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("filter by key", g_filter, sizeof(g_filter));
    ImGui::SameLine();
    ImGui::Checkbox("static", &g_showStatic);
    ImGui::SameLine();
    ImGui::Checkbox("moving", &g_showMoving);
    ImGui::SameLine();
    ImGui::Checkbox("blinked off", &g_showDark);
    ImGui::SameLine();
    ImGui::Checkbox("overridden only", &g_onlyOverridden);
    ImGui::SameLine();
    ImGui::Checkbox("not drawn only", &g_onlyUndrawn);

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
    if (!ImGui::BeginTable("lights", 8, flags, ImVec2(0.0f, 320.0f))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("col", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("position (x, up, z)", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("int", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("reach", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 90.0f);
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
                           ImVec2(18.0f, 18.0f));

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
        ImGui::TextColored(kDim, "Select a light above to edit it. Lights that move have no "
                                 "stable identity and cannot be given a saved override -- use "
                                 "the moving-light scale on the Tuning tab for those.");
        return;
    }

    ImGui::Text("Selected %016llX", static_cast<unsigned long long>(g_selected));
    const RemixTrackedLight* t = FindTracked(g_selected);
    if (t) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, "-- colour %.2f %.2f %.2f, intensity %.2f, reach %.2f tiles",
                           t->def.rgb[0], t->def.rgb[1], t->def.rgb[2], t->def.intensity,
                           t->def.radius);
    } else {
        ImGui::SameLine();
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

    if (existing) {
        if (ImGui::Button("Reset this light")) {
            LightsEraseOverride(g_selected);
            LightsInvalidate(g_selected);
        }
    }
}

void DrawSavedOverrides() {
    ImGui::TextColored(kDim, "Every saved override, including ones belonging to other districts.");
    if (ImGui::Button("Clear all overrides")) {
        LightsClearOverrides();
        LightsInvalidate(0);
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("overrides", 4, flags, ImVec2(0.0f, 240.0f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("in view", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, 220.0f);
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
        if (ov.intensity != 1.0f) _snprintf(intensity, sizeof(intensity) - 1, "x%.2f ", ov.intensity);
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

}  // namespace

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

    g_initialised = true;
    Log("F4 menu ready (input on %p, drawn into %p)", inputWindow, presentWindow);
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

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(940.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (g_resetWindowPos) {
        g_resetWindowPos = false;
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f));
    }
    if (ImGui::Begin("GTA2 -- RTX Remix (F4)")) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Status")) {
                DrawStatus();
                ImGui::Separator();
                DrawFrameRate();
                ImGui::Separator();
                DrawTuning();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("ToD")) {
                DrawTimeOfDay();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Invented")) {
                DrawSynthetic();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Effects")) {
                DrawEffectInspector();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sprites")) {
                DrawSprites();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Lights")) {
                DrawMapLightClasses();
                ImGui::Separator();
                DrawLightTable();
                ImGui::Separator();
                DrawSelectedEditor();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Overrides")) {
                DrawSavedOverrides();
                ImGui::Separator();
                DrawSelectedEditor();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    // Anything the menu touched this frame is unsaved. Cheaper and more honest
    // than trying to mark every individual widget.
    if (ImGui::GetIO().WantCaptureMouse && ImGui::IsAnyItemActive()) SettingsMarkDirty();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace gta2dx9
