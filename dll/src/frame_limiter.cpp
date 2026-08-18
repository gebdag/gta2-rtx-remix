#include "frame_limiter.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmsystem.h>

#include <cstdio>

namespace gta2dx9 {
namespace {

float g_targetFps = 0.0f;
bool  g_periodRaised = false;

LARGE_INTEGER g_frequency = {};
LARGE_INTEGER g_nextFrame = {};   // when this frame is allowed to end
bool g_haveDeadline = false;

// Measured, over a second, so a menu reading does not flicker with every frame.
double g_measuredFps = 0.0;
double g_idleFraction = 0.0;
LARGE_INTEGER g_windowStart = {};
int    g_windowFrames = 0;
double g_windowIdleSeconds = 0.0;

// Sleep(1) with the default 15.6 ms timer resolution overshoots a 16.7 ms frame
// by most of a frame, so the period is raised while a cap is in force. Released
// again when the cap goes away, because it is a process-wide setting and holding
// it costs the whole machine a little power.
void SetPeriod(bool want) {
    if (want == g_periodRaised) return;
    if (want) {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) g_periodRaised = true;
    } else {
        timeEndPeriod(1);
        g_periodRaised = false;
    }
}

double Seconds(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    if (!g_frequency.QuadPart) return 0.0;
    return static_cast<double>(to.QuadPart - from.QuadPart) /
           static_cast<double>(g_frequency.QuadPart);
}

}  // namespace

void FrameLimitSet(float fps) {
    if (fps > 0.0f && fps < 10.0f) fps = 10.0f;
    if (fps > 1000.0f) fps = 1000.0f;
    if (fps < 0.0f) fps = 0.0f;
    if (fps == g_targetFps) return;
    g_targetFps = fps;
    g_haveDeadline = false;   // a new period starts from now, not from the old deadline
    SetPeriod(g_targetFps > 0.0f);
    if (g_targetFps > 0.0f) {
        Log("frame limiter: %.1f fps (game speed about %.2fx, GTA2's own step being 33 ms)",
            g_targetFps, g_targetFps / 30.3f);
    } else {
        Log("frame limiter: off");
    }
}

float FrameLimitFps() { return g_targetFps; }
float FrameLimitMeasuredFps() { return static_cast<float>(g_measuredFps); }
float FrameLimitIdleFraction() { return static_cast<float>(g_idleFraction); }

void FrameLimitWait() {
    if (!g_frequency.QuadPart) QueryPerformanceFrequency(&g_frequency);
    if (!g_frequency.QuadPart) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LARGE_INTEGER arrived = now;

    if (g_targetFps > 0.0f) {
        const LONGLONG period =
            static_cast<LONGLONG>(static_cast<double>(g_frequency.QuadPart) / g_targetFps);

        if (!g_haveDeadline) {
            g_nextFrame.QuadPart = now.QuadPart + period;
            g_haveDeadline = true;
        } else {
            // Sleep gives back control a millisecond or so late at best, so the
            // last stretch is spun out. Kept to under 2 ms of spinning, which is
            // a few percent of one core rather than the whole of it.
            const LONGLONG spinMargin = g_frequency.QuadPart / 500;   // 2 ms
            while (now.QuadPart < g_nextFrame.QuadPart) {
                const LONGLONG remaining = g_nextFrame.QuadPart - now.QuadPart;
                if (remaining > spinMargin) {
                    const DWORD ms = static_cast<DWORD>(
                        ((remaining - spinMargin) * 1000) / g_frequency.QuadPart);
                    if (ms > 0) Sleep(ms);
                    else YieldProcessor();
                } else {
                    YieldProcessor();
                }
                QueryPerformanceCounter(&now);
            }
            g_nextFrame.QuadPart += period;
            // A frame that overran badly - a level load, a device reset - would
            // otherwise leave the deadline in the past and be paid back by a
            // burst of uncapped frames. Start again from now instead.
            if (g_nextFrame.QuadPart < now.QuadPart) g_nextFrame.QuadPart = now.QuadPart + period;
        }
    } else {
        g_haveDeadline = false;
    }

    ++g_windowFrames;
    g_windowIdleSeconds += Seconds(arrived, now);
    if (!g_windowStart.QuadPart) g_windowStart = now;
    const double elapsed = Seconds(g_windowStart, now);
    if (elapsed >= 1.0) {
        g_measuredFps = g_windowFrames / elapsed;
        g_idleFraction = g_windowIdleSeconds / elapsed;
        g_windowFrames = 0;
        g_windowIdleSeconds = 0.0;
        g_windowStart = now;
    }
}

void FrameLimitShutdown() { SetPeriod(false); }

}  // namespace gta2dx9
