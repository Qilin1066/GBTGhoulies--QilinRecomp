// The I3DL2 reverb, natively: on the console the APU's DSP runs it from the
// game's effects image, driven by DSI3DL2LISTENER (docs/audio.md, "Effects").
// The model is I3DL2's own: the input through the room level and its
// high-frequency roll-off, early reflections after the reflections delay,
// and a late reverb after the reverb delay that decays by the decay time
// (faster at high frequencies, by the HF ratio), diffused by allpasses and
// made dense by eight feedback delay lines; four outputs, decorrelated, to
// the front and back speakers.
#include <algorithm>
#include <cmath>

#include "audio.hpp"

namespace qilin::audio {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRate = float(kOutputRate);

// A one-pole low-pass y = (1 − a)·x + a·y[−1] whose gain at frequency f is g (≤ 1).
float one_pole_for(float g, float f) {
    g = std::clamp(g, 0.0001f, 1.0f);
    if (g >= 0.9999f) return 0.0f;
    const float c = std::cos(2.0f * kPi * std::min(f, kRate * 0.49f) / kRate);
    const float g2 = g * g;
    const float b = 1.0f - g2 * c, d = 1.0f - g2;
    const float disc = std::max(0.0f, b * b - d * d);
    return std::clamp((b - std::sqrt(disc)) / d, 0.0f, 0.9999f);
}

uint32_t frames_for(float seconds) { return uint32_t(std::max(0.0f, seconds) * kRate + 0.5f); }

}  // namespace

float I3dl2Reverb::Allpass::run(float x) {
    const float delayed = buf[pos];
    const float v = x + g * delayed;
    buf[pos] = v;
    pos = pos + 1 == buf.size() ? 0 : pos + 1;
    return delayed - g * v;
}

I3dl2Reverb::I3dl2Reverb() {
    // Line lengths in milliseconds: mutually prime-ish, 30 to 70 ms.
    static constexpr float kLineMs[kLines] = {29.7f, 37.1f, 41.1f, 43.7f, 47.9f, 53.3f, 59.9f, 67.1f};
    for (uint32_t i = 0; i < kLines; ++i) lines_[i].buf.assign(frames_for(kLineMs[i] / 1000.0f * 1.0f), 0.0f);
    diffusers_[0].buf.assign(frames_for(0.0047f), 0.0f);
    diffusers_[1].buf.assign(frames_for(0.0113f), 0.0f);
    pre_.assign(frames_for(0.3f + 0.1f + 0.1f) + 1, 0.0f);  // the longest reflections delay and reverb delay, and the taps' spread
}

void I3dl2Reverb::set(const I3dl2Params& p) {
    p_ = p;
    on_ = p.room > kVolumeMin;
    in_gain_ = gain(p.room);
    in_lp_ = one_pole_for(gain(std::min(p.room_hf, 0)), p.hf_reference);
    // Reflections to +10 dB and reverb to +20 dB, as I3DL2 allows.
    reflections_gain_ = p.reflections <= kVolumeMin ? 0.0f : float(std::pow(10.0, double(std::min(p.reflections, 1000)) / 2000.0));
    late_gain_ = p.reverb <= kVolumeMin ? 0.0f : float(std::pow(10.0, double(std::min(p.reverb, 2000)) / 2000.0));
    // The reflections: four taps spread over 1-10 ms after the delay
    // (wider with less density); the late reverb from the delay after them.
    const float spread = 0.001f + 0.009f * (1.0f - std::clamp(p.density, 0.0f, 100.0f) / 100.0f * 0.5f);
    const float r0 = std::clamp(p.reflections_delay, 0.0f, 0.3f);
    for (uint32_t t = 0; t < 4; ++t) reflection_taps_[t] = frames_for(r0 + spread * float(t) / 3.0f);
    late_delay_ = frames_for(r0 + std::clamp(p.reverb_delay, 0.0f, 0.1f));
    const float g = 0.7f * std::clamp(p.diffusion, 0.0f, 100.0f) / 100.0f;
    diffusers_[0].g = diffusers_[1].g = g;
    // Each line's feedback: −60 dB over the decay time at low frequencies,
    // over decay time × HF ratio at the HF reference.
    const float t60 = std::clamp(p.decay_time, 0.1f, 20.0f), t60_hf = t60 * std::clamp(p.decay_hf_ratio, 0.1f, 2.0f);
    for (Line& l : lines_) {
        const float seconds = float(l.buf.size()) / kRate;
        l.gain = std::pow(10.0f, -3.0f * seconds / t60);
        const float hf = std::pow(10.0f, -3.0f * seconds / t60_hf);
        l.lp = hf < l.gain ? one_pole_for(hf / l.gain, p.hf_reference) : 0.0f;
    }
}

void I3dl2Reverb::process(Bins& bins) {
    if (!on_) return;
    const uint32_t n = bins.frames;
    const float* in = bins.bin(kI3dl2);
    float* out[4] = {bins.bin(kFrontLeft), bins.bin(kFrontRight), bins.bin(kBackLeft), bins.bin(kBackRight)};
    const uint32_t size = uint32_t(pre_.size());
    auto tap = [&](uint32_t back) { return pre_[(pre_pos_ + size - std::min(back, size - 1)) % size]; };
    // The late reverb's level: the lines' sum carries about √8 of the input.
    const float late = late_gain_ * 0.35f;
    for (uint32_t i = 0; i < n; ++i) {
        // The room: level, then the high frequencies' roll-off.
        in_lp_state_ = (1.0f - in_lp_) * in[i] * in_gain_ + in_lp_ * in_lp_state_;
        pre_[pre_pos_] = in_lp_state_;
        // Early reflections, one tap to each output.
        float o[4];
        for (uint32_t t = 0; t < 4; ++t) o[t] = tap(reflection_taps_[t]) * reflections_gain_;
        // Late reverb: diffused, into the feedback network (a Householder
        // mix: each line takes its own output less a quarter of the sum).
        const float x = diffusers_[1].run(diffusers_[0].run(tap(late_delay_)));
        float y[kLines], sum = 0.0f;
        for (uint32_t k = 0; k < kLines; ++k) y[k] = lines_[k].buf[lines_[k].pos], sum += y[k];
        for (uint32_t k = 0; k < kLines; ++k) {
            Line& l = lines_[k];
            float v = (y[k] - sum * (2.0f / kLines)) * l.gain;
            l.state = (1.0f - l.lp) * v + l.lp * l.state;  // the high frequencies decay faster
            l.buf[l.pos] = l.state + x;
            l.pos = l.pos + 1 == l.buf.size() ? 0 : l.pos + 1;
        }
        o[0] += (y[0] + y[4]) * late;
        o[1] += (y[1] + y[5]) * late;
        o[2] += (y[2] + y[6]) * late;
        o[3] += (y[3] + y[7]) * late;
        for (uint32_t t = 0; t < 4; ++t) out[t][i] += o[t];
        pre_pos_ = pre_pos_ + 1 == size ? 0 : pre_pos_ + 1;
    }
}

}  // namespace qilin::audio
