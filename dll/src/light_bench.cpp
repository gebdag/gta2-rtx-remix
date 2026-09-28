#include "light_bench.h"

#include "frame_limiter.h"
#include "game_access.h"
#include "log.h"
#include "remix_lights.h"

#include "../../src/gta2_map.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gta2dx9 {
namespace {

// Lights added per step. 0 is the baseline: the game's own lights and ours,
// as they are, so every other step reads as a difference from it.
const int kCounts[] = {0, 25, 50, 100, 200, 400, 800, 1600, 3200, 6400};
const int kStepCount = static_cast<int>(sizeof(kCounts) / sizeof(kCounts[0]));

// Frames given to a step before it is measured - long enough for the new
// lights to have been created and the bridge to have settled - and measured.
const int kWarmFrames = 20;
const int kMeasureFrames = 60;

// A step this slow is past anything playable, and the next one would only be
// slower; the pass stops there.
const double kGiveUpMs = 200.0;

enum Pass { kStatic = 0, kMoving = 1, kPassCount = 2 };
const char* const kPassNames[kPassCount] = {"static", "moving"};

struct Step {
    int pass = 0;
    int count = 0;
    int frames = 0;
    double intervalSum = 0.0, intervalMax = 0.0;
    double workSum = 0.0;          // interval less the limiter's idle share
    double reconcileSum = 0.0, reconcileMax = 0.0;
    double drawSum = 0.0, drawMax = 0.0;
    int tracked = 0, drawn = 0;
    unsigned failuresAtStart = 0, failures = 0;
    int lastError = 0;
};

bool g_running = false;
int g_pass = 0;
int g_step = 0;
int g_frameInStep = 0;
unsigned g_frame = 0;
LARGE_INTEGER g_lastRecord = {};
std::vector<Step> g_results;
Step g_current;
char g_status[160] = "Not run yet.";

double Now() {
    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

void BeginStep() {
    g_current = Step();
    g_current.pass = g_pass;
    g_current.count = kCounts[g_step];
    g_current.failuresAtStart = LightsStats().applyFailures;
    g_frameInStep = 0;
    snprintf(g_status, sizeof(g_status), "Running: %s lights, %d of them (step %d of %d)...",
             kPassNames[g_pass], g_current.count, g_pass * kStepCount + g_step + 1,
             kPassCount * kStepCount);
}

void WriteResults() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) slash[1] = '\0';
    strncat(path, "gta2dx9_lightbench.csv", MAX_PATH - strlen(path) - 1);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "pass,added_lights,tracked,drawn,frames,interval_ms_avg,interval_ms_max,"
                   "work_ms_avg,reconcile_ms_avg,reconcile_ms_max,draw_ms_avg,draw_ms_max,"
                   "create_failures,last_error\n");
    }
    for (const Step& s : g_results) {
        const double n = s.frames ? static_cast<double>(s.frames) : 1.0;
        Log("light bench: %-6s +%5d lights (tracked %5d, drawn %5d) | frame %6.2f ms avg "
            "%6.2f max | work %6.2f ms | reconcile %6.2f avg %6.2f max | draw %6.2f avg %6.2f "
            "max | failures %u%s",
            kPassNames[s.pass], s.count, s.tracked, s.drawn, s.intervalSum / n, s.intervalMax,
            s.workSum / n, s.reconcileSum / n, s.reconcileMax, s.drawSum / n, s.drawMax,
            s.failures, s.failures ? " <- Remix refused lights" : "");
        if (f) {
            fprintf(f, "%s,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%d\n",
                    kPassNames[s.pass], s.count, s.tracked, s.drawn, s.frames,
                    s.intervalSum / n, s.intervalMax, s.workSum / n, s.reconcileSum / n,
                    s.reconcileMax, s.drawSum / n, s.drawMax, s.failures, s.lastError);
        }
    }
    if (f) fclose(f);
    Log("light bench: results written to %s", path);
}

void Finish(const char* why) {
    g_running = false;
    WriteResults();
    snprintf(g_status, sizeof(g_status), "%s - %d steps measured, see gta2dx9_lightbench.csv.",
             why, static_cast<int>(g_results.size()));
    Log("light bench: %s", why);
}

}  // namespace

void LightBenchStart() {
    if (g_running) return;
    g_results.clear();
    g_pass = kStatic;
    g_step = 0;
    g_running = true;
    g_lastRecord.QuadPart = 0;
    Log("light bench: started (fps cap %.0f; work = frame time less the limiter's idle share)",
        FrameLimitFps());
    BeginStep();
}

void LightBenchStop() {
    if (g_running) Finish("Stopped by hand");
}

bool LightBenchRunning() { return g_running; }

const char* LightBenchStatus() { return g_status; }

void LightBenchSubmit() {
    if (!g_running) return;
    ++g_frame;
    const int count = kCounts[g_step];
    if (count == 0) return;

    float camX = 0.0f, camY = 0.0f;
    if (!game::CameraPosition(&camX, &camY)) return;

    // A square grid over the few blocks around the camera, a little above street
    // level. Dim enough not to change the picture much: this measures the
    // bridge, not the look.
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const float span = 6.0f;
    const float spacing = span / static_cast<float>(side);
    const float wobble = g_pass == kMoving ? 0.02f * std::sin(g_frame * 0.7f) : 0.0f;
    for (int i = 0; i < count; ++i) {
        const float gx = camX - span * 0.5f + (i % side + 0.5f) * spacing + wobble;
        const float gy = camY - span * 0.5f + (i / side + 0.5f) * spacing - wobble;
        RemixLightDesc d;
        // The same frame conversion the synthetic lights use: x east, y up, z
        // north with the rows mirrored.
        d.pos[0] = gx;
        d.pos[1] = 3.2f;
        d.pos[2] = static_cast<float>(gta2::kMapHeight) - gy;
        d.rgb[0] = 1.0f;
        d.rgb[1] = 1.0f;
        d.rgb[2] = 1.0f;
        d.intensity = 0.0005f;
        d.radius = 1.0f;
        d.emitterRadius = 0.02f;
        d.source = kLightSourceSpark;
        d.explicitId = 0xBE4C000000000000ULL | static_cast<uint64_t>(i + 1);
        LightsSubmitExtra(d);
    }
}

void LightBenchRecord(double reconcileMs, double drawMs) {
    if (!g_running) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const double now = Now();
    static double last = 0.0;
    const double interval = g_lastRecord.QuadPart ? now - last : 0.0;
    g_lastRecord = t;
    last = now;

    ++g_frameInStep;
    if (g_frameInStep <= kWarmFrames || interval <= 0.0) return;

    Step& s = g_current;
    ++s.frames;
    s.intervalSum += interval;
    if (interval > s.intervalMax) s.intervalMax = interval;
    s.workSum += interval * (1.0 - static_cast<double>(FrameLimitIdleFraction()));
    s.reconcileSum += reconcileMs;
    if (reconcileMs > s.reconcileMax) s.reconcileMax = reconcileMs;
    s.drawSum += drawMs;
    if (drawMs > s.drawMax) s.drawMax = drawMs;
    const RemixLightStats& st = LightsStats();
    s.tracked = st.tracked;
    s.drawn = st.drawnLastFrame;
    s.failures = st.applyFailures - s.failuresAtStart;
    s.lastError = st.lastApplyError;

    if (s.frames < kMeasureFrames) return;

    g_results.push_back(s);
    const double avg = s.intervalSum / s.frames;
    const bool broke = s.failures > 0 || avg > kGiveUpMs;
    if (broke) {
        Log("light bench: %s pass stops at %d lights (%s)", kPassNames[g_pass], s.count,
            s.failures ? "Remix refused lights" : "frame too slow");
    }
    if (broke || ++g_step >= kStepCount) {
        g_step = 0;
        if (++g_pass >= kPassCount) {
            Finish("Finished");
            return;
        }
    }
    BeginStep();
}

}  // namespace gta2dx9
