#include "time_of_day.h"

#include "log.h"
#include "remix_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gta2dx9 {
namespace {

TimeOfDaySettings g_settings;
float g_hour = 2.0f;

float g_elevation = -30.0f;
float g_rotation = 0.0f;

bool  g_pushed = false;
int   g_pushes = 0;
char  g_status[128] = "not started";

LARGE_INTEGER g_frequency = {};
LARGE_INTEGER g_lastTick = {};
LARGE_INTEGER g_lastPush = {};
bool g_haveTick = false;

// Last values actually sent, so an unchanged sky costs nothing.
float g_sentElevation = 1e9f;
float g_sentRotation = 1e9f;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

double Seconds(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    if (!g_frequency.QuadPart) return 0.0;
    return static_cast<double>(to.QuadPart - from.QuadPart) /
           static_cast<double>(g_frequency.QuadPart);
}

void SetStatus(const char* text) {
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
}

bool Push(const char* key, float degrees) {
    char value[32];
    _snprintf(value, sizeof(value) - 1, "%.4f", degrees);
    value[sizeof(value) - 1] = '\0';
    return RemixSetConfig(key, value);
}

}  // namespace

TimeOfDaySettings& TimeOfDay() { return g_settings; }

float TimeOfDayHour() { return g_hour; }

void TimeOfDaySetHour(float hour) {
    hour = std::fmod(hour, 24.0f);
    if (hour < 0.0f) hour += 24.0f;
    g_hour = hour;
}

void TimeOfDayReset() { TimeOfDaySetHour(g_settings.startHour); }

// Where the sun is, from the standard solar position equations.
//
// With latitude phi, solar declination delta and hour angle H (zero at local
// noon, 15 degrees per hour, positive in the afternoon):
//
//     sin(altitude) = sin(phi) sin(delta) + cos(phi) cos(delta) cos(H)
//     cos(azimuth)  = (sin(delta) - sin(phi) sin(altitude)) / (cos(phi) cos(altitude))
//
// with the azimuth measured clockwise from north and mirrored about noon, which
// is what puts sunrise in the east. Nothing here is approximated: this is the
// same geometry a sundial uses, and it is why the night is as long as it is and
// why the sun barely clears the horizon in a northern winter.
void TimeOfDaySunAt(float hour, const TimeOfDaySettings& settings, float* elevationDeg,
                    float* rotationDeg) {
    const float phi = settings.latitudeDeg * kDegToRad;
    const float decl = settings.declinationDeg * kDegToRad;
    const float H = (hour - 12.0f) * 15.0f * kDegToRad;

    const float sinAlt = std::sin(phi) * std::sin(decl) +
                         std::cos(phi) * std::cos(decl) * std::cos(H);
    const float altitude = std::asin(sinAlt < -1.0f ? -1.0f : (sinAlt > 1.0f ? 1.0f : sinAlt));

    float azimuth = 0.0f;
    const float cosAlt = std::cos(altitude);
    // At the poles, and at the instant the sun is straight overhead, the azimuth
    // is undefined rather than merely awkward. Due south is the harmless answer.
    if (std::fabs(cosAlt) > 1e-5f && std::fabs(std::cos(phi)) > 1e-5f) {
        float cosAz = (std::sin(decl) - std::sin(phi) * sinAlt) / (std::cos(phi) * cosAlt);
        if (cosAz < -1.0f) cosAz = -1.0f;
        if (cosAz > 1.0f) cosAz = 1.0f;
        azimuth = std::acos(cosAz) * kRadToDeg;   // 0 = north, 180 = south
        if (H > 0.0f) azimuth = 360.0f - azimuth;  // afternoon is the western half
    } else {
        azimuth = 180.0f;
    }

    if (!settings.rotationClockwise) azimuth = 360.0f - azimuth;
    azimuth += settings.rotationOffsetDeg;
    azimuth = std::fmod(azimuth, 360.0f);
    if (azimuth < 0.0f) azimuth += 360.0f;

    float elevation = altitude * kRadToDeg + settings.elevationOffsetDeg;
    if (elevation < -90.0f) elevation = -90.0f;
    if (elevation > 90.0f) elevation = 90.0f;

    if (elevationDeg) *elevationDeg = elevation;
    if (rotationDeg) *rotationDeg = azimuth;
}

void TimeOfDayUpdate() {
    if (!g_frequency.QuadPart) QueryPerformanceFrequency(&g_frequency);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_haveTick) {
        g_lastTick = now;
        g_lastPush = now;
        g_haveTick = true;
        TimeOfDaySetHour(g_settings.startHour);
    }
    const double delta = Seconds(g_lastTick, now);
    g_lastTick = now;

    if (!g_settings.enabled) {
        SetStatus("off");
        return;
    }

    if (!g_settings.paused) {
        // A level load can stall a frame for seconds; letting that run the clock
        // would jump the sun across the sky on the frame the map appears.
        const double step = delta > 1.0 ? 1.0 : delta;
        TimeOfDaySetHour(g_hour +
                         static_cast<float>(step * g_settings.minutesPerSecond / 60.0));
    }

    TimeOfDaySunAt(g_hour, g_settings, &g_elevation, &g_rotation);

    if (!RemixApiAvailable()) {
        g_pushed = false;
        SetStatus("Remix API not available; the clock runs but the sky does not move");
        return;
    }

    const double sincePush = Seconds(g_lastPush, now) * 1000.0;
    const bool due = sincePush >= g_settings.pushIntervalMs;
    const bool moved = std::fabs(g_elevation - g_sentElevation) > 0.005f ||
                       std::fabs(g_rotation - g_sentRotation) > 0.005f;
    if (!due || !moved) return;
    g_lastPush = now;

    const bool okElevation = Push("rtx.atmosphere.sunElevation", g_elevation);
    const bool okRotation = Push("rtx.atmosphere.sunRotation", g_rotation);
    g_pushed = okElevation && okRotation;
    if (g_pushed) {
        g_sentElevation = g_elevation;
        g_sentRotation = g_rotation;
        ++g_pushes;
        if (g_pushes == 1) {
            Log("time of day: driving rtx.atmosphere.sunElevation/sunRotation "
                "(start %05.2f, %.0f game minutes per real second)",
                g_settings.startHour, g_settings.minutesPerSecond);
        }
        SetStatus("driving the Remix sun");
    } else {
        SetStatus("SetConfigVariable was refused - is this Remix build recent enough?");
    }
}

float DaylightGateFactorAt(const DaylightGate& gate, float sunElevationDeg) {
    if (!gate.enabled) return 1.0f;
    // A gate the wrong way round would divide by zero and is more likely a
    // mistyped slider than an intention, so it is read as a hard switch.
    if (gate.offAboveDeg <= gate.onBelowDeg) {
        return sunElevationDeg >= gate.offAboveDeg ? 0.0f : 1.0f;
    }
    if (sunElevationDeg >= gate.offAboveDeg) return 0.0f;
    if (sunElevationDeg <= gate.onBelowDeg) return 1.0f;
    const float t = (gate.offAboveDeg - sunElevationDeg) / (gate.offAboveDeg - gate.onBelowDeg);
    // Smoothstep rather than a straight ramp: the ends are what the eye notices,
    // and a linear fade visibly starts and stops.
    return t * t * (3.0f - 2.0f * t);
}

float DaylightGateFactor(const DaylightGate& gate) {
    if (!gate.enabled || !g_settings.enabled) return 1.0f;
    return DaylightGateFactorAt(gate, g_elevation);
}

void TimeOfDayAngles(float* elevationDeg, float* rotationDeg) {
    if (elevationDeg) *elevationDeg = g_elevation;
    if (rotationDeg) *rotationDeg = g_rotation;
}

bool TimeOfDayPushed() { return g_pushed; }
int TimeOfDayPushCount() { return g_pushes; }
const char* TimeOfDayStatus() { return g_status; }

}  // namespace gta2dx9
