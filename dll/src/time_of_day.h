// A day and night cycle, driven into RTX Remix's atmosphere.
//
// Remix places its sun from two config variables, both in degrees and both
// documented in the runtime as "Game-drivable per-frame":
//
//     rtx.atmosphere.sunElevation    height above the horizon, -90 .. +90
//     rtx.atmosphere.sunRotation     compass direction, 0 .. 360
//
// They are pushed through remixapi_Interface::SetConfigVariable, which is the
// supported way for a game to move the sky.
//
// The two angles are not invented. They come from the standard solar position
// equations for a latitude and a solar declination, so the sun rises in the
// east, sets in the west, climbs highest at local noon, and spends the night
// below the horizon by the amount it actually would - which is what makes 2 am
// dark rather than merely dim. The season is a single number (the declination,
// or the day of the year that produces it) and the latitude is a slider, so the
// city can be anywhere from the tropics to somewhere with a real winter.
//
// GTA2's Anywhere City is nowhere in particular, so the defaults are a temperate
// northern latitude at the equinox: twelve hours of daylight, a sun that reaches
// 50 degrees at noon, and long slanted shadows morning and evening.
#pragma once

namespace gta2dx9 {

struct TimeOfDaySettings {
    bool enabled = true;

    // Where the clock starts when a level begins. 2.0 = 02:00, which is deep
    // night at every latitude and season below.
    float startHour = 2.0f;

    // How fast the clock runs, in game minutes per real second. 1 is one real
    // second to one game minute, so a full day takes 1440 real seconds - 24 real
    // minutes.
    float minutesPerSecond = 1.0f;
    bool  paused = false;

    // Where on Earth this city is. Latitude sets how high the sun climbs and how
    // steeply it rises; declination sets the season (+23.44 at the June
    // solstice, 0 at either equinox, -23.44 in December).
    float latitudeDeg = 40.0f;
    float declinationDeg = 0.0f;

    // Remix's own reference for sunRotation is not documented, and a city's
    // streets do not have to run north-south anyway, so the compass bearing can
    // be turned and, if it comes out mirrored, flipped.
    float rotationOffsetDeg = 0.0f;
    bool  rotationClockwise = true;

    // A thumb on the scale for elevation, for pulling the night up out of pitch
    // black or pushing midday down. Applied after the astronomy.
    float elevationOffsetDeg = 0.0f;

    // Each push is a round trip across the 32-bit Remix bridge, and the sun
    // moves by a fraction of a degree per frame, so it is not worth doing every
    // frame. 50 ms is twenty pushes a second and far smoother than the eye.
    float pushIntervalMs = 50.0f;
};

TimeOfDaySettings& TimeOfDay();

// Advances the clock and pushes the sun. Call once per frame.
void TimeOfDayUpdate();

// Back to startHour. Called when a level begins, so every run starts at 2 am.
void TimeOfDayReset();

// The clock, 0 .. 24.
float TimeOfDayHour();
void  TimeOfDaySetHour(float hour);

// What was last computed, whether or not it reached Remix.
void TimeOfDayAngles(float* elevationDeg, float* rotationDeg);

// Whether the last push went through, and how many have.
bool        TimeOfDayPushed();
int         TimeOfDayPushCount();
const char* TimeOfDayStatus();

// The astronomy on its own, so the menu can draw the day's curve without
// disturbing the clock.
void TimeOfDaySunAt(float hour, const TimeOfDaySettings& settings, float* elevationDeg,
                    float* rotationDeg);

// --- Lights that only exist after dark ------------------------------------
//
// GTA2 has no day, so every street lamp, neon sign and headlight in it burns at
// noon. That was invisible while the sky never changed and is glaring once it
// does, so a light can be told to follow the sun.
//
// Expressed as the two elevations that bracket the switch-on rather than as a
// clock time, because that is what actually decides it: the same lamp comes on
// later in June than in December, and at a different hour at a different
// latitude, and reading it off the sun gets all of that for free. The defaults
// are the real ones - street lighting and headlights go on around sunset and are
// fully on by the end of civil twilight, six degrees down.
struct DaylightGate {
    bool  enabled = false;
    float offAboveDeg = 0.0f;    // out by the time the sun is this high
    float onBelowDeg = -6.0f;    // fully lit once the sun is this far down
};

// How much of this light should be burning right now, 0..1, eased rather than
// switched so nothing pops. Always 1 when the gate is off or the cycle is not
// running: a light nobody asked to follow the sun keeps burning, which is the
// behaviour this had before gates existed.
float DaylightGateFactor(const DaylightGate& gate);

// The same curve for an arbitrary sun elevation, so the menu can show what a
// gate will do across the whole day without waiting for it.
float DaylightGateFactorAt(const DaylightGate& gate, float sunElevationDeg);

}  // namespace gta2dx9
