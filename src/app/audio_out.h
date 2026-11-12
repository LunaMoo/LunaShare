// One sound output for the whole app.
// On Android SDL plays through OpenSL ES, which gives the app a single playback device: while one is open - even
// paused - opening another fails ("Audio device already open"). Three things play sound: calls (MediaHost), music and
// radio (music::Player) and audio / video files (PlayerHost). Each used to open its own device, so whichever was
// second stayed silent: a call after a paused video, the radio after a voice message...
// They go through here, so that failure is seen (and can be tried on a computer: LUNASHARE_ONE_OUTPUT=1 refuses a
// second device the way a phone does), and they follow one order: a call has the output first (music and files are
// mixed into it), then a file that plays, then music; whoever doesn't play lets go, whoever couldn't open tries again.
#pragma once
#include <SDL.h>

namespace audio_out {
bool Single();  // the app gets one output device (phones)
SDL_AudioDeviceID Open(const char* who, const char* device, const SDL_AudioSpec* want, SDL_AudioSpec* have);
void Close(SDL_AudioDeviceID dev);
const char* Holder();  // who has it ("" = nobody): for the log
}  // namespace audio_out
