// Sound output, through SDL3: the mixer's 48 kHz float frames to the
// default playback device (docs/audio.md, "Output").
//
// The mixer runs on the audio clock (the host's steady clock), catching up
// whenever the game reaches DirectSound; what it mixes is queued here. The
// queue starts with a cushion of silence, so the device never runs dry
// between catch-ups, and its length is held there by trimming the
// playback rate a little (at most 0.5%, inaudible), which absorbs the
// drift between the device's clock and the host's.
#pragma once

#include <cstdint>

struct SDL_AudioStream;

namespace qilin::platform {

class AudioOut {
public:
    // Opens the default playback device for `channels` channels at `rate`:
    // 2 (stereo) or 6 (5.1: front left, right, centre, LFE, back left,
    // right), or 0 for the device's own (6 where it has that many). False
    // (with a message printed) where there's no sound device; the game then
    // runs silent.
    bool start(uint32_t rate, uint32_t channels);
    ~AudioOut();

    // Interleaved float frames, in the channel order given to start().
    void push(const float* frames, uint32_t count);

    uint32_t channels() const { return channels_; }

private:
    ::SDL_AudioStream* stream_ = nullptr;
    uint32_t rate_ = 0, channels_ = 0;
    uint32_t target_frames_ = 0;  // the cushion held queued
    float ratio_ = 1.0f;
};

}  // namespace qilin::platform
