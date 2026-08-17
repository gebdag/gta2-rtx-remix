#include "debug_overlay.h"

#include "game_access.h"
#include "live_geometry.h"
#include "log.h"
#include "remix_api.h"
#include "remix_lights.h"
#include "synthetic_lights.h"

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

void DrawStatus() {
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
                ImGui::TextColored(kDim, "From the vehicle list. A car counts as driven while it "
                                         "has moved recently, which covers the player and the "
                                         "traffic and leaves parked cars dark.");
                bool changed = false;
                int window = static_cast<int>(s.drivenWindowMs);
                changed |= ImGui::SliderInt("Driven window (ms)", &window, 0, 10000);
                s.drivenWindowMs = static_cast<unsigned>(window);
                ImGui::SetItemTooltip("How long a car keeps its beams after it stops moving. Long "
                                      "enough that waiting at a junction does not switch them "
                                      "off.");
                changed |= ImGui::SliderFloat("Movement threshold", &s.drivenMinMovement, 0.0f,
                                              0.05f, "%.4f");
                if (changed) SyntheticLightsMarkDirty();
            } else if (boundTypes == 0) {
                ImGui::TextColored(kBad, "No particle type is bound to this category, so it emits "
                                         "nothing. Go to the Effects tab, trigger the effect in "
                                         "game, and bind the id that appears.");
            } else {
                ImGui::TextColored(kGood, "%d particle type(s) bound.", boundTypes);
            }
            ImGui::Text("emitted last frame: %d", st.emitted[i]);
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
        "seconds are highlighted.");

    ImGui::Spacing();
    if (ImGui::Button("Clear list")) SyntheticForgetParticleTypes();
    ImGui::SameLine();
    const SyntheticStats& st = SyntheticLightsStats();
    ImGui::TextColored(kDim, "%d type(s) seen, %d particle(s) live",
                       static_cast<int>(SyntheticParticleTypes().size()), st.particlesWalked);

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("types", 6, flags, ImVec2(0.0f, 380.0f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 54.0f);
    ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("seen", ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("life", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("last position", ImGuiTableColumnFlags_WidthFixed, 180.0f);
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
        ImGui::Text("%u", info.totalSeen);
        ImGui::TableNextColumn();
        ImGui::Text("%d", info.lastLife);
        ImGui::TableNextColumn();
        ImGui::Text("%7.2f %5.2f %7.2f", info.lastPos[0], info.lastPos[1], info.lastPos[2]);

        ImGui::TableNextColumn();
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

void DrawSprites() {
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
                DrawTuning();
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

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace gta2dx9
