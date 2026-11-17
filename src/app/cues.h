// Short sounds for the chat, made here (no sound files): a message sent, one that comes, someone starting to write.
// Computers play them on a sound output of their own. A phone gives the app one output (audio_out.h): while a call,
// music or a file has it, they're mixed into that one's sound (MixS16 / MixF32 from its audio callback).
#pragma once
#include <cstdint>

namespace cues {
enum class Cue {
    Sent,          // a message I sent: a light rising blip
    Received,      // a message came in a chat that isn't open: two notes, "pop-ding"
    ReceivedOpen,  // ...in the chat that's open: the same, softer
    Typing,        // someone started writing in the open chat: two soft taps
};
void Play(Cue c);
void Tick();  // (UI thread, once a frame: lets go of its own output when nothing has played for a while)
// from another audio callback (one output on phones): adds what plays now
void MixS16(int16_t* mono, int frames, int rate);
void MixF32(float* out, int frames, int channels, int rate);
}  // namespace cues
