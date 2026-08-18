// A frame rate cap of our own.
//
// GTA2 has one already, and it is a checkbox rather than a number. Its pacer
// (gta2.exe!FUN_00462A30) compares timeGetTime() against a deadline it advances
// by a hardcoded 33 ms per frame - 0x0045A460 returns 33, or 11 in the network
// game - and the two registry values the manager writes, max_frame_rate and
// min_frame_rate, are read at 0x004CB1D0 as plain booleans into 0x00595010 and
// 0x00673598. There is no number in the registry to raise: capped means 33 ms,
// uncapped means the pacer stops waiting entirely.
//
// So the cap is ours. The game's own is left off, its loop free-runs, and this
// holds the frame at whatever period is asked for.
//
// What that costs, stated plainly: GTA2 advances its simulation exactly one step
// per rendered frame. The pacer's 33 ms is that step, and nothing in the game
// scales motion by elapsed time - which is precisely why uncapped runs fast
// rather than merely smooth. A cap of N therefore runs the game at N/30.3 times
// its intended speed:
//
//     30 fps   1.0x   the original speed, and the original 30 fps
//     45 fps   1.5x
//     60 fps   2.0x   smooth, and visibly brisk
//
// There is no setting that gives 60 fps of motion at 1x speed; that would need
// the game's per-step constants halved, which is not something a renderer can
// do. The number is here so the trade is yours to make rather than a checkbox's.
#pragma once

namespace gta2dx9 {

// 0 disables the cap and lets the game free-run. Clamped to a sane band
// otherwise: below 10 the game is unplayable and above 1000 the limiter is
// measuring its own overhead.
void  FrameLimitSet(float fps);
float FrameLimitFps();

// Sleeps out the rest of this frame's budget. Call once per frame, at the very
// end, after the flip.
void FrameLimitWait();

// What the last second of frames actually ran at, for the menu.
float FrameLimitMeasuredFps();

// How much of the last frame was spent waiting, as a fraction. Near zero means
// the cap is not the thing limiting the frame rate.
float FrameLimitIdleFraction();

// Drops the timer period request taken while a cap was in force.
void FrameLimitShutdown();

}  // namespace gta2dx9
