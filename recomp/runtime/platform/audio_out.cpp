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

std::vector<std::string> AudioOut::playback_devices() {
    std::vector<std::string> names;
    const bool was_on = SDL_WasInit(SDL_INIT_AUDIO) != 0;
    if (!was_on && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return names;
    int n = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) {
        for (int i = 0; i < n; ++i)
            if (const char* name = SDL_GetAudioDeviceName(ids[i])) names.emplace_back(name);
        SDL_free(ids);
    }
    if (!was_on) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}

bool AudioOut::start(uint32_t rate, uint32_t channels, const std::string& device) {
    if (stream_) return true;
    if (!rate || rate > 384000 || (channels != 0 && channels != 2 && channels != 6)) return false;
    rate_ = rate;
    channels_ = channels ? channels : 2;
    device_ = device;
    retry_at_ = SDL_GetTicks() + 1000;
    // A small device buffer (about 10 ms at 48 kHz): the cushion queued here
    // already covers the time between catch-ups, so a big one only adds delay.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::printf("qilin_run: no sound (SDL: %s)\n", SDL_GetError());
        return false;
    }
    // The device asked for by name, else the system's default.
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!device.empty()) {
        int n = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
        for (int i = 0; ids && i < n; ++i)
            if (const char* name = SDL_GetAudioDeviceName(ids[i]); name && device == name) id = ids[i];
        SDL_free(ids);
        if (id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
            std::printf("qilin_run: the sound device in the settings (%s) isn't there; using the system's default\n", device.c_str());
    }
    // 0 channels: the device's own layout, 5.1 where it has six or more
    // speakers (the Xbox's own mix), else stereo.
    if (channels == 0) {
        SDL_AudioSpec dev{};
        channels = SDL_GetAudioDeviceFormat(id, &dev, nullptr) && dev.channels >= 6 ? 6 : 2;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = int(channels);
    spec.freq = int(rate);
    stream_ = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (!stream_) {
        std::printf("qilin_run: no sound (can't open the playback device: %s)\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    rate_ = rate, channels_ = channels;
    ratio_ = 1.0f; full_since_ = 0;
    target_frames_ = rate * kCushionMs / 1000;
    const std::vector<float> silence(size_t(target_frames_) * channels, 0.0f);
    if (!SDL_PutAudioStreamData(stream_, silence.data(), int(silence.size() * sizeof(float))) ||
        !SDL_ResumeAudioStreamDevice(stream_)) { failed(); return false; }
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
    std::printf("qilin_run: sound: %u Hz, %u channels, to %s%s\n", rate, channels, name ? name : "the default device",
                id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? " (the system's default: followed if it changes)" : " (as the settings say)");
    return true;
}

AudioOut::~AudioOut() { stop(); }

void AudioOut::stop() {
    if (!stream_) return;
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
void AudioOut::failed() {
    std::printf("qilin_run: sound output interrupted (%s); retrying in one second\n", SDL_GetError());
    stop();
    retry_at_ = SDL_GetTicks() + 1000;
}

int AudioOut::queued_frames() const {
    if (!stream_) return -1;
    const int bytes = SDL_GetAudioStreamQueued(stream_);
    return bytes < 0 ? -1 : bytes / int(channels_ * sizeof(float));
}

void AudioOut::push(const float* frames, uint32_t count) {
    if (!count) return;
    if (!stream_ && rate_ && SDL_GetTicks() >= retry_at_) start(rate_, channels_, device_);
    if (!stream_) { dropped_ += count; return; }
    // After a stall (the game loading without reaching DirectSound or a
    // swap), the mixer catches up all at once: what's older than the queue's
    // limit is already past, as it would have played on the console, so only
    // the newest frames are queued.
    const int before = SDL_GetAudioStreamQueued(stream_);
    if (before < 0) { dropped_ += count; failed(); return; }
    const uint32_t queued_frames = before > 0 ? uint32_t(before) / (channels_ * uint32_t(sizeof(float))) : 0;
    const uint32_t limit = target_frames_ * 4, room = queued_frames < limit ? limit - queued_frames : 0;
    if (!room) {
        const uint64_t now = SDL_GetTicks();
        if (!full_since_) full_since_ = now;
        else if (now - full_since_ >= 1000) {
            dropped_ += count;
            SDL_SetError("playback device stopped consuming samples"); failed(); return;
        }
    } else full_since_ = 0;
    if (count > room) dropped_ += count - room, frames += size_t(count - room) * channels_, count = room;
    if (!count) return;
    if (!SDL_PutAudioStreamData(stream_, frames, int(size_t(count) * channels_ * sizeof(float)))) {
        dropped_ += count; failed(); return;
    }
    pushed_ += count;
    // Hold the queue at the cushion: a longer queue plays a little faster,
    // a shorter one a little slower. The trim moves slowly, so it follows the
    // clocks' drift, not each catch-up's step.
    const int queued_bytes = SDL_GetAudioStreamQueued(stream_);
    if (queued_bytes < 0) { failed(); return; }
    const float queued = float(queued_bytes) / float(channels_ * sizeof(float));
    const float error = (queued - float(target_frames_)) / float(target_frames_);
    const float want = 1.0f + std::clamp(error * 0.01f, -kMaxTrim, kMaxTrim);
    ratio_ += (want - ratio_) * 0.05f;
    if (!SDL_SetAudioStreamFrequencyRatio(stream_, ratio_)) failed();
}

}  // namespace qilin::platform
