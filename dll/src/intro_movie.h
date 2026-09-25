// The intro movie, played or skipped, and in either case never through
// DirectDraw. See intro_movie.cpp.
#pragma once

namespace gta2dx9 {

// The game's own default: the intro plays.
constexpr bool kDefaultIntro = true;

// Hands one decoded frame of the movie to whoever draws it: 32-bit B,G,R,X rows,
// `pitch` bytes apart.
using MovieFramePresenter = void (*)(const void* pixels, int width, int height, int pitch);

// Patches gta2.exe so that `play` decides whether the intro plays, in place of
// the do_play_movie registry value, and so that when it does play each frame is
// handed to `present` rather than drawn by Bink with DirectDraw. Must run before
// the game's startup check for the movie, which gbh_InitDLL does.
//
// A gta2.exe that is not the build these addresses came from is left alone.
void IntroInstall(bool play, MovieFramePresenter present);

}  // namespace gta2dx9
