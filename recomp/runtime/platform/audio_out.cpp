#include "audio_out.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace qilin::platform {

namespace {
constexpr uint32_t kCushionMs = 60;     // queued ahead of the device: two of the game's 30 Hz frames, with room
constexpr float kMaxTrim = 0.005f;      // the most the playback rate is trimmed by, either way
}  // namespace

bool AudioOut::start(uint32_t rate, uint32_t channels) {
    if (stream_) return true;
    // A small device buffer (about 10 ms at 48 kHz): the cushion queued here
    // already covers the time between catch-ups, so a big one only adds delay.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::printf("qilin_run: no sound (SDL: %s)\n", SDL_GetError());
        return false;
    }
    // 0 channels: the device's own layout, 5.1 where it has six or more
    // speakers (the Xbox's own mix), else stereo.
    if (channels == 0) {
        SDL_AudioSpec dev{};
        channels = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, nullptr) && dev.channels >= 6 ? 6 : 2;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = int(channels);
    spec.freq = int(rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        std::printf("qilin_run: no sound (can't open the playback device: %s)\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    rate_ = rate, channels_ = channels;
    target_frames_ = rate * kCushionMs / 1000;
    const std::vector<float> silence(size_t(target_frames_) * channels, 0.0f);
    SDL_PutAudioStreamData(stream_, silence.data(), int(silence.size() * sizeof(float)));
    SDL_ResumeAudioStreamDevice(stream_);
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
    std::printf("qilin_run: sound: %u Hz, %u channels, to %s\n", rate, channels, name ? name : "the default device");
    return true;
}

AudioOut::~AudioOut() {
    if (!stream_) return;
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::push(const float* frames, uint32_t count) {
    if (!stream_ || !count) return;
    // After a stall (the game loading without reaching DirectSound or a
    // swap), the mixer catches up all at once: what's older than the queue's
    // limit is already past, as it would have played on the console, so only
    // the newest frames are queued.
    const int before = SDL_GetAudioStreamQueued(stream_);
    const uint32_t queued_frames = before > 0 ? uint32_t(before) / (channels_ * uint32_t(sizeof(float))) : 0;
    const uint32_t limit = target_frames_ * 4, room = queued_frames < limit ? limit - queued_frames : 0;
    if (count > room) frames += size_t(count - room) * channels_, count = room;
    if (!count) return;
    SDL_PutAudioStreamData(stream_, frames, int(size_t(count) * channels_ * sizeof(float)));
    // Hold the queue at the cushion: a longer queue plays a little faster,
    // a shorter one a little slower. The trim moves slowly, so it follows the
    // clocks' drift, not each catch-up's step.
    const int queued_bytes = SDL_GetAudioStreamQueued(stream_);
    if (queued_bytes < 0) return;
    const float queued = float(queued_bytes) / float(channels_ * sizeof(float));
    const float error = (queued - float(target_frames_)) / float(target_frames_);
    const float want = 1.0f + std::clamp(error * 0.01f, -kMaxTrim, kMaxTrim);
    ratio_ += (want - ratio_) * 0.05f;
    SDL_SetAudioStreamFrequencyRatio(stream_, ratio_);
}

}  // namespace qilin::platform
