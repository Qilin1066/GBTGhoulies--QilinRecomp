// Voices: decoding PCM and Xbox ADPCM, resampling, gain and mix bins.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio.hpp"

namespace qilin::audio {

uint32_t Format::block_bytes() const {
    if (codec == Codec::XboxAdpcm) return 36 * channels;
    return channels * bits / 8;
}

int32_t pitch_for_rate(uint32_t rate) {
    return rate == 0 ? 0 : int32_t(std::lround(4096.0 * std::log2(double(rate) / kOutputRate)));
}

namespace {

constexpr int16_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int8_t kIndexAdjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

}  // namespace

float gain(int32_t millibels) {
    if (millibels <= kVolumeMin) return 0.0f;
    return float(std::pow(10.0, double(std::min(millibels, 0)) / 2000.0));
}

uint32_t attenuation_gain(uint32_t att) {
    static const auto table = [] {
        std::array<uint32_t, 49> t{};
        for (uint32_t k = 0; k <= 48; ++k) t[k] = uint32_t(std::floor(4095.0 * std::pow(10.0, -double(k) / 20.0)));
        return t;
    }();
    if (att == 0) return 16384;
    if (att >= 0xFC0) return 0;
    const uint32_t k = att / 64, frac = att % 64;
    if (k >= 48) return table[48] * 4 * (0xFC0 - att) / (0xFC0 - 48 * 64);  // to 0 at 63 dB
    const uint32_t a = k == 0 ? 16384 : table[k] * 4, b = table[k + 1] * 4;
    return (a * (64 - frac) + b * frac) / 64;
}

uint32_t attenuation_for(int64_t millibels) {
    if (millibels >= 0) return 0;
    return uint32_t(std::min<int64_t>((-millibels) * 64 / 100, 0xFFF));
}

float hrtf_tap(uint8_t t) {
    const uint32_t e = t >> 4 & 7, m = t & 15;
    const float v = e ? float(16 + m) * float(1u << e) / 2.0f / 2048.0f : float(m) / 2048.0f;
    return t & 0x80 ? -v : v;
}

namespace {
// 1/16 of a 24-bit sample (full scale 1.0 = 2^23 units), halves up.
inline double round_step(double x) { return std::floor(x * 134217728.0 + 0.5) / 134217728.0; }
}  // namespace

double Svf::frequency(uint32_t fc) {
    const double octaves = std::round(double(int16_t(uint16_t(fc))) / 4.0) / 1024.0;
    return std::min(65535.0, std::round(std::pow(2.0, octaves) * 65536.0)) / 65536.0;
}

// The filter is unstable at the top of its range with heavy damping (f
// near 1, damping near 2: it grows without bound in about 30 ms). The
// hardware's registers are fixed point and can't grow without bound; here
// the state is held to ±16 (well past full scale, which the measurements
// say the filter's output can pass), so such a voice is loud, not infinite,
// and can't turn the whole mix into infinities.
double Svf::step(double x, double f, double damping) {
    constexpr double kLimit = 16.0;
    low = std::clamp(round_step(low + f * band), -kLimit, kLimit);
    const double high = std::clamp(round_step(x - low - damping * band), -kLimit, kLimit);
    band = std::clamp(round_step(f * high + band), -kLimit, kLimit);
    return high;
}

// IMA ADPCM with the Xbox's layout: per channel, a 4-byte header (the
// predictor as a signed 16-bit sample, the step index, a reserved byte),
// then the nibbles in 4-byte groups (8 samples each) interleaved across
// channels, low nibble first. The header sample seeds the predictor; the 64
// nibbles are the block's 64 samples (DirectSound's 64 samples per block).
void decode_xbox_adpcm_block(const uint8_t* block, uint32_t channels, int16_t out[][64]) {
    for (uint32_t ch = 0; ch < channels; ++ch) {
        const uint8_t* h = block + 4 * ch;
        int32_t predictor = int16_t(uint16_t(h[0] | (h[1] << 8)));
        int32_t index = std::clamp<int32_t>(h[2], 0, 88);
        const uint8_t* data = block + 4 * channels;
        for (uint32_t group = 0; group < 8; ++group) {
            const uint8_t* g = data + (group * channels + ch) * 4;
            for (uint32_t n = 0; n < 8; ++n) {
                const uint32_t nibble = (g[n / 2] >> ((n & 1) * 4)) & 0xF;
                const int32_t step = kStep[index];
                int32_t diff = step >> 3;
                if (nibble & 1) diff += step >> 2;
                if (nibble & 2) diff += step >> 1;
                if (nibble & 4) diff += step;
                predictor = std::clamp(nibble & 8 ? predictor - diff : predictor + diff, -32768, 32767);
                index = std::clamp<int32_t>(index + kIndexAdjust[nibble & 7], 0, 88);
                out[ch][group * 8 + n] = int16_t(predictor);
            }
        }
    }
}

void Voice::set_data(uint32_t guest_addr, uint32_t bytes) {
    data_ = guest_addr;
    bytes_ = bytes;
    cached_block_ = UINT64_MAX;
    play_start_ = 0;
    play_len_ = 0;
    loop_start_ = 0;
    loop_len_ = 0;
    pos_ = 0;
}

void Voice::set_format(const Format& f) {
    format_ = f;
    cached_block_ = UINT64_MAX;
    pitch = pitch_for_rate(f.rate);
}

uint64_t Voice::region_end() const {
    const uint64_t total = format_.bytes_to_frames(bytes_);
    return std::min(total, play_len_ ? play_start_ + play_len_ : total);
}

void Voice::set_play_region(uint32_t start, uint32_t length) {
    play_start_ = format_.bytes_to_frames(start);
    play_len_ = format_.bytes_to_frames(length);
}

void Voice::set_loop_region(uint32_t start, uint32_t length) {
    loop_start_ = format_.bytes_to_frames(start);
    loop_len_ = format_.bytes_to_frames(length);
}

void Voice::set_position(uint32_t bytes) { pos_ = double(format_.bytes_to_frames(bytes)); }

uint32_t Voice::position_bytes() const { return uint32_t(format_.frames_to_bytes(uint64_t(pos_))); }

void Voice::play(bool looping) {
    looping_ = looping;
    const bool was = playing_;
    if (!playing_ && (pos_ < double(play_start_) || pos_ >= double(region_end()))) pos_ = double(play_start_);
    playing_ = bytes_ != 0 || submix_;
    if (playing_ && !was) start_envelope(), fresh_ = true;
}

void Voice::stop() { playing_ = false; }

// ---------------------------------------------------------------- envelope and filter
void Voice::set_envelope(const Envelope& e) {
    const bool was_off = eg_.mode == 0;
    eg_ = e;
    if (e.mode == 0) stage_ = Stage::Off, level_ = 1.0f;
    else if (was_off && playing_ && stage_ == Stage::Off) start_envelope();
}

void Voice::start_envelope() {
    stage_frames_ = 0;
    switch (eg_.mode) {
        case 1: stage_ = Stage::Delay, level_ = 0.0f; break;
        case 2: stage_ = Stage::Attack, level_ = 0.0f; break;
        case 3: stage_ = Stage::Hold, level_ = 1.0f; break;
        default: stage_ = Stage::Off, level_ = 1.0f; break;
    }
}

void Voice::release() {
    if (stage_ == Stage::Off || !playing_) {
        stop();
        return;
    }
    if (stage_ == Stage::Release) return;
    stage_ = Stage::Release, stage_frames_ = 0, release_from_ = level_;
}

float Voice::envelope_step() {
    constexpr uint64_t kUnit = 512;  // frames per envelope time unit
    // Each stage runs its length in frames, then hands the frame on to the
    // next (a zero-length stage takes none).
    for (;;) {
        const uint64_t n = stage_frames_;
        auto next = [&](Stage s) { stage_ = s, stage_frames_ = 0; };
        switch (stage_) {
            case Stage::Off: return 1.0f;
            case Stage::Delay:
                if (n >= eg_.delay * kUnit) { next(Stage::Attack); continue; }
                level_ = 0.0f;
                break;
            case Stage::Attack: {
                const uint64_t t = eg_.attack * kUnit;
                if (n >= t) { level_ = 1.0f, next(Stage::Hold); continue; }
                level_ = float(n + 1) / float(t);
                break;
            }
            case Stage::Hold:
                if (n >= eg_.hold * kUnit) { next(Stage::Decay); continue; }
                level_ = 1.0f;
                break;
            case Stage::Decay: {
                // To about -64 dB over the decay time (the APU's curve), or the sustain level.
                const float sustain = float(eg_.sustain & 0xFF) / 255.0f;
                const uint64_t t = eg_.decay * kUnit;
                if (n >= t) { next(Stage::Sustain); continue; }
                level_ = std::max(sustain, std::exp(-7.34f * float(n + 1) / float(t)));
                if (level_ <= sustain) { next(Stage::Sustain); continue; }
                break;
            }
            case Stage::Sustain: level_ = float(eg_.sustain & 0xFF) / 255.0f; return level_;
            case Stage::Release: {
                const uint64_t t = eg_.release * kUnit;
                if (n + 1 >= t) {
                    level_ = 0.0f;
                    stage_ = Stage::Off;
                    playing_ = false;  // the release over, the voice stops
                    return 0.0f;
                }
                level_ = release_from_ * (1.0f - float(n + 1) / float(t));
                break;
            }
        }
        ++stage_frames_;
        return level_;
    }
}

void Voice::set_filter(const Filter& f) {
    if (f.mode != filter_.mode) {
        for (Svf* v : {&dls2_[0], &dls2_[1], &peq_[0], &peq_[1]}) *v = Svf{};
        i3dl2_[0] = i3dl2_[1] = 0;
    }
    filter_ = f;
}

// One frame as left/right floats (mono: the same in both).
void Voice::decode_frame(const uint8_t* mem, size_t mem_size, uint64_t frame, float* lr) {
    const uint32_t ch = format_.channels;
    uint64_t base = data_, local = frame;  // where the frame's data starts, and its index there
    if (stream_) {
        const Packet* p = nullptr;
        for (const Packet& q : packets_)
            if (frame >= q.first && frame < q.first + q.frames) p = &q;
        if (!p) {
            lr[0] = lr[1] = 0;
            return;
        }
        base = p->addr, local = frame - p->first;
    }
    if (format_.codec == Format::Codec::XboxAdpcm) {
        const uint64_t block = frame / 64;  // packets hold whole blocks, so this numbers blocks across them
        const uint64_t at = base + (local / 64) * format_.block_bytes();
        if (block != cached_block_) {
            if (at + format_.block_bytes() > mem_size) {
                lr[0] = lr[1] = 0;
                return;
            }
            int16_t out[2][64];
            decode_xbox_adpcm_block(mem + at, ch, out);
            for (uint32_t c = 0; c < ch; ++c) std::copy(out[c], out[c] + 64, block_[c].begin());
            cached_block_ = block;
        }
        lr[0] = float(block_[0][frame % 64]) / 32768.0f;
        lr[1] = float(block_[ch - 1][frame % 64]) / 32768.0f;
        return;
    }
    const uint64_t at = base + local * format_.block_bytes();
    if (at + format_.block_bytes() > mem_size) {
        lr[0] = lr[1] = 0;
        return;
    }
    for (uint32_t c = 0; c < 2; ++c) {
        const uint32_t k = std::min(c, ch - 1);
        if (format_.bits == 8) lr[c] = (float(mem[at + k]) - 128.0f) / 128.0f;
        else lr[c] = float(int16_t(uint16_t(mem[at + 2 * k] | (mem[at + 2 * k + 1] << 8)))) / 32768.0f;
    }
}

namespace {

bool left_side(uint32_t bin) { return bin == kFrontLeft || bin == kBackLeft || bin == kXtlkFrontLeft || bin == kXtlkBackLeft; }
bool right_side(uint32_t bin) { return bin == kFrontRight || bin == kBackRight || bin == kXtlkFrontRight || bin == kXtlkBackRight; }

}  // namespace

bool Voice::process(const uint8_t* mem, size_t mem_size, uint32_t frames) {
    if (!playing_ || (bytes_ == 0 && !submix_ && !stream_)) return false;
    const uint32_t ch = format_.channels;
    // The source, resampled (a 3D voice's Doppler offset on its pitch).
    std::vector<float> inter(size_t(frames) * ch, 0.0f);
    const float identity[2][2] = {{1, 0}, {0, 1}};
    const int32_t own_pitch = pitch;
    if (three_d.active) pitch += three_d.pitch_offset;
    render(mem, mem_size, inter.data(), ch, identity, frames);
    pitch = own_pitch;
    scratch_.assign(size_t(frames) * ch, 0.0f);
    // The envelope.
    for (uint32_t i = 0; i < frames; ++i) {
        const float g = stage_ == Stage::Off ? 1.0f : envelope_step();
        for (uint32_t c = 0; c < ch; ++c) scratch_[size_t(c) * frames + i] = inter[size_t(i) * ch + c] * g;
        if (!playing_ && stage_ == Stage::Off && eg_.mode) break;  // released to silence
    }
    // The voice filter, as the hardware runs it by mode (measured on a
    // retail console, xemu pull request 3105; Filter says where the words
    // come from). Mono: 1 the DLS2 low-pass from FCA, 2 the parametric EQ
    // from FCB, 3 both. Stereo: channel 0 from FCA, 1 from FCB (3 as 1; not
    // measured). A 3D voice with a head-related filter: 1 the DLS2 or 2 the
    // EQ from FCA, and in every mode the I3DL2 pair from FCB (DirectSound
    // makes its mode 0 a 3), a direct path for routes 0-4 and 6 and a room
    // path for 5 and 7. The state-variable filter's steps run low, high,
    // band each sample, at 48 kHz (Chamberlin's): the order of xemu's
    // filter, which the measurements corrected (they removed its input
    // scale, sqrt(q/2 + 0.01), and its cubic waveshaper, which are those of
    // the Chamberlin filter it came from) and were matched against.
    const bool hrtf3d = three_d.active && three_d.hrtf && ch == 1;
    const uint32_t mode = hrtf3d && (filter_.mode & 3) == 0 ? 3 : filter_.mode & 3;
    auto dls2 = [&](uint32_t c, uint32_t word) {
        const double f = Svf::frequency(word & 0xFFFF), damping = double(word >> 16) / 32768.0;
        float* row = &scratch_[size_t(c) * frames];
        for (uint32_t i = 0; i < frames; ++i) dls2_[c].step(row[i], f, damping), row[i] = float(dls2_[c].low);
    };
    auto peq = [&](uint32_t c, uint32_t word) {
        const double f = Svf::frequency(word & 0xFFFF), g = double(int16_t(uint16_t(word >> 16))) / 8192.0;
        const double q = std::ldexp(1.0, -int(filter_.q & 7));
        float* row = &scratch_[size_t(c) * frames];
        for (uint32_t i = 0; i < frames; ++i) {
            peq_[c].step(row[i], f, q);
            row[i] = float(round_step(row[i] + g * q * peq_[c].band));
        }
    };
    room_.clear();
    if (hrtf3d) {
        if (mode == 1) dls2(0, filter_.fca());
        if (mode == 2) peq(0, filter_.fca());
        const uint32_t fcb = three_d.fcb_set ? three_d.fcb : filter_.fcb();
        const double direct = double(fcb & 0xFFFF) / 65536.0, room = double(fcb >> 16) / 65536.0;
        room_.resize(frames);
        float* row = &scratch_[0];
        for (uint32_t i = 0; i < frames; ++i) {
            i3dl2_[1] = round_step((1.0 - room) * row[i] + room * i3dl2_[1]);
            i3dl2_[0] = round_step((1.0 - direct) * row[i] + direct * i3dl2_[0]);
            room_[i] = float(i3dl2_[1]);
            row[i] = float(i3dl2_[0]);
        }
    } else if (ch == 1) {
        if (mode & 1) dls2(0, filter_.fca());
        if (mode & 2) peq(0, filter_.fcb());
    } else if (mode) {
        for (uint32_t c = 0; c < ch; ++c) {
            const uint32_t word = c ? filter_.fcb() : filter_.fca();
            if (mode == 2) peq(c, word);
            else dls2(c, word);
        }
    }
    // A 3D voice's head-related filter: 31 taps a ear, the hardware's
    // minifloats applied as written (hrtf_tap), the far ear delayed by the
    // interaural delay.
    if (three_d.active && three_d.hrtf && ch == 1) {
        const uint32_t itd = uint32_t(std::min(std::abs(three_d.itd), 63));
        const size_t hist = 30 + 63;
        if (hrtf_history_.size() != hist) hrtf_history_.assign(hist, 0.0f);
        std::vector<float> ext(hist + frames);
        std::copy(hrtf_history_.begin(), hrtf_history_.end(), ext.begin());
        std::copy(scratch_.begin(), scratch_.begin() + frames, ext.begin() + hist);
        float taps[2][31], energy[2] = {};
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 31; ++k) taps[e][k] = hrtf_tap(three_d.taps[e][k]), energy[e] += taps[e][k] * taps[e][k];
        // The delay's sign names the ear, as DirectSound's lookup makes it
        // (the left table's delay past the azimuth split, else the right's
        // negated): positive delays the left ear. (Checked against the
        // filters over five minutes of play: in 7.96 million blocks with a
        // delay, the delayed ear's filter was the quieter one every time.)
        const int far = three_d.itd > 0 ? 0 : 1;
        (void)energy;
        hrtf_out_.assign(size_t(frames) * 2, 0.0f);
        for (int e = 0; e < 2; ++e) {
            const uint32_t delay = e == far ? itd : 0;
            float* o = &hrtf_out_[size_t(e) * frames];
            for (uint32_t i = 0; i < frames; ++i) {
                float acc = 0.0f;
                const float* x = &ext[hist + i - delay];
                for (int k = 0; k < 31; ++k) acc += taps[e][k] * x[-k];
                o[i] = acc;
            }
        }
        std::copy(ext.end() - long(hist), ext.end(), hrtf_history_.begin());
    }
    return true;
}

void Voice::mix(const uint8_t* mem, size_t mem_size, Bins& out, uint32_t frames, Voice* submix) {
    if (!process(mem, mem_size, frames)) return;
    if (submix) feed(submix, frames);
    // Each route: the bin it feeds at the voice's volume plus the route's
    // (and a 3D voice's offset for its bin), through the APU's attenuation
    // curve. A gain moves from the last block's in 32 steps over the first
    // frame (32 samples), as the hardware ramps it, and a voice just
    // started fades in from silence. A 3D voice with a head-related filter
    // sends its left ear on routes 0-1, its right on 2-3, the rest of its
    // signal on 4-7 (DirectSound orders its bins so: the crosstalk
    // canceller's left inputs, then its right); the room path (I3DL2) on 5 and 7.
    const uint32_t ch = format_.channels;
    std::vector<MixBinVolume> bins = mix_bins;
    if (bins.empty()) bins = {{kFrontLeft, 0}, {kFrontRight, 0}};
    const bool hrtf = three_d.active && three_d.hrtf && ch == 1 && !hrtf_out_.empty();
    const bool by_route = hrtf && bins.size() >= 4;
    if (route_gain_.size() != bins.size()) route_gain_.assign(bins.size(), -1.0f);  // -1: no ramp (unless fresh)
    for (size_t k = 0; k < bins.size(); ++k) {
        const uint32_t bin = bins[k].bin;
        if (bin >= kMixBinCount) continue;
        const uint32_t c = ch == 1 ? 0 : uint32_t(k % ch);
        const int64_t mb = int64_t(volume) + bins[k].volume + (three_d.active ? three_d.bin_offset[bin] : 0);
        const float g = float(attenuation_gain(attenuation_for(mb))) / 16384.0f;
        const float from = fresh_ ? 0.0f : route_gain_[k] < 0 ? g : route_gain_[k];
        route_gain_[k] = g;
        if (g == 0.0f && from == 0.0f) continue;
        const float* src = &scratch_[size_t(c) * frames];
        if (by_route) {
            if (k < 2) src = &hrtf_out_[0];
            else if (k < 4) src = &hrtf_out_[frames];
            else if ((k == 5 || k == 7) && !room_.empty()) src = room_.data();
        } else if (hrtf && left_side(bin)) {
            src = &hrtf_out_[0];
        } else if (hrtf && right_side(bin)) {
            src = &hrtf_out_[frames];
        }
        float* dst = out.bin(bin);
        const uint32_t ramp = std::min<uint32_t>(frames, from == g ? 0 : 32);
        for (uint32_t i = 0; i < ramp; ++i) dst[i] += src[i] * (from + (g - from) * float(i) / 32.0f);
        for (uint32_t i = ramp; i < frames; ++i) dst[i] += src[i] * g;
    }
    fresh_ = false;
}

void Voice::make_submix(uint32_t channels) {
    submix_ = true;
    format_ = Format{};
    format_.channels = channels;
    format_.rate = kOutputRate;
    playing_ = true;
}

void Voice::mix_into(const uint8_t* mem, size_t mem_size, Voice& submix, uint32_t frames) {
    if (bytes_ == 0 && !stream_) return;
    if (!process(mem, mem_size, frames)) return;
    feed(&submix, frames);
}

void Voice::feed(Voice* target, uint32_t frames) {
    Voice& submix = *target;
    const float g = float(attenuation_gain(attenuation_for(volume))) / 16384.0f;
    const uint32_t dst = submix.format_.channels, ch = format_.channels;
    if (submix.input_.size() < size_t(frames) * dst) submix.input_.resize(size_t(frames) * dst, 0.0f);
    for (uint32_t c = 0; c < ch; ++c) {
        const float* row = &scratch_[size_t(c) * frames];
        for (uint32_t d = 0; d < dst; ++d) {
            if (dst != 1 && ch != 1 && c != d) continue;  // stereo into stereo: channel for channel
            for (uint32_t i = 0; i < frames; ++i) submix.input_[size_t(i) * dst + d] += row[i] * g;
        }
    }
}

namespace {
// The hardware's resampler (measured on a retail console, xemu pull request
// 3105): 8-point Lagrange interpolation over the frames at −3 … +4 around
// the cursor, the weights rounded to 1/65536; the cursor 16.16 fixed point,
// advancing 2^(pitch/4096) a sample.
void lagrange8(double t, float w[8]) {
    static constexpr double kDenominator[8] = {-5040, 720, -240, 144, -144, 240, -720, 5040};  // Π (j − k), k ≠ j, nodes −3 … 4
    double prefix[9], suffix[9];
    prefix[0] = 1, suffix[8] = 1;
    for (int j = 0; j < 8; ++j) prefix[j + 1] = prefix[j] * (t - double(j - 3));
    for (int j = 7; j >= 0; --j) suffix[j] = suffix[j + 1] * (t - double(j - 3));
    for (int j = 0; j < 8; ++j) w[j] = float(std::round(prefix[j] * suffix[j + 1] / kDenominator[j] * 65536.0) / 65536.0);
}
double cursor_step(int32_t pitch) { return std::round(std::pow(2.0, double(pitch) / 4096.0) * 65536.0) / 65536.0; }
// One output frame at frame f0 + t (t in [0, 1)): frame_at(n, lr) gives frame n.
template <class FrameAt>
void resample(uint64_t f0, double t, FrameAt&& frame_at, float s[2]) {
    if (t == 0.0) return frame_at(int64_t(f0), s);
    float w[8];
    lagrange8(t, w);
    s[0] = s[1] = 0;
    for (int j = 0; j < 8; ++j) {
        if (w[j] == 0.0f) continue;
        float lr[2];
        frame_at(int64_t(f0) + j - 3, lr);
        s[0] += w[j] * lr[0], s[1] += w[j] * lr[1];
    }
}
}  // namespace

void Voice::recent_frame(const uint8_t* mem, size_t mem_size, uint64_t frame, float* lr) {
    RecentFrame& r = recent_[frame & 15];
    if (r.frame != frame) {
        decode_frame(mem, mem_size, frame, r.lr);
        r.frame = frame;
    }
    lr[0] = r.lr[0], lr[1] = r.lr[1];
}

void Voice::render(const uint8_t* mem, size_t mem_size, float* out, uint32_t dst, const float to[2][2], uint32_t frames) {
    const uint32_t ch = format_.channels;
    if (submix_) {  // its input, frame for frame
        for (uint32_t i = 0; i < frames && size_t(i) * ch < input_.size(); ++i)
            for (uint32_t c = 0; c < ch; ++c)
                for (uint32_t d = 0; d < dst; ++d) out[dst * i + d] += input_[size_t(i) * ch + c] * to[c][d];
        return;
    }
    if (stream_) return render_stream(mem, mem_size, out, dst, to, frames);
    forget_recent();
    const double step = cursor_step(pitch);
    const uint64_t end = region_end();
    const uint64_t loop_start = looping_ ? std::max(loop_start_, play_start_) : 0;
    const uint64_t loop_end = looping_ ? (loop_len_ ? std::min(end, loop_start + loop_len_) : end) : end;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t limit = looping_ ? loop_end : end;
        if (pos_ >= double(limit)) {
            if (!looping_ || loop_end <= loop_start) {
                playing_ = false;
                pos_ = double(end);
                return;
            }
            pos_ = double(loop_start) + std::fmod(pos_ - double(loop_start), double(loop_end - loop_start));
        }
        // The frames around the cursor: past the end a loop wraps, else
        // silence; before the play region's start, silence.
        auto frame_at = [&](int64_t n, float* lr) {
            if (n >= int64_t(limit) && looping_) n = int64_t(loop_start) + (n - int64_t(loop_start)) % int64_t(loop_end - loop_start);
            if (n < int64_t(play_start_) || n >= int64_t(limit)) lr[0] = lr[1] = 0;
            else recent_frame(mem, mem_size, uint64_t(n), lr);
        };
        const uint64_t f0 = uint64_t(pos_);
        float s[2];
        resample(f0, pos_ - double(f0), frame_at, s);
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t d = 0; d < dst; ++d) out[dst * i + d] += s[c] * to[c][d];
        pos_ += step;
    }
}

// ---------------------------------------------------------------- streaming
void Voice::make_stream() {
    stream_ = true;
    forget_recent();
    packets_.clear();
    completed_.clear();
    queued_end_ = 0;
    pos_ = 0;
}

void Voice::queue_packet(uint32_t guest_addr, uint32_t bytes, uint32_t id) {
    const uint64_t frames = format_.bytes_to_frames(bytes);
    packets_.push_back({guest_addr, bytes, id, queued_end_, frames});
    // A voice stopped (after a discontinuity, or never started) starts again
    // at the new packet: the frames since the last one are silence, not data;
    // its envelope starts again with it.
    if (!playing_) pos_ = double(queued_end_), start_envelope(), fresh_ = true;
    queued_end_ += frames;
    playing_ = true;
    discontinuity_ = false;
}

void Voice::retire_played() {
    while (!packets_.empty() && packets_.front().first + packets_.front().frames <= uint64_t(pos_)) {
        completed_.push_back({packets_.front().id, packets_.front().bytes});
        packets_.pop_front();
    }
}

std::vector<Voice::Done> Voice::take_completed() {
    std::vector<Done> v;
    v.swap(completed_);
    return v;
}

std::vector<Voice::Done> Voice::flush() {
    std::vector<Done> v;
    for (const Packet& p : packets_) {
        const uint64_t played = uint64_t(pos_) > p.first ? std::min<uint64_t>(uint64_t(pos_) - p.first, p.frames) : 0;
        v.push_back({p.id, uint32_t(format_.frames_to_bytes(played))});
    }
    packets_.clear();
    pos_ = double(queued_end_);
    playing_ = false;
    return v;
}

void Voice::render_stream(const uint8_t* mem, size_t mem_size, float* out, uint32_t dst, const float to[2][2], uint32_t frames) {
    if (paused_) return;
    const uint32_t ch = format_.channels;
    const double step = cursor_step(pitch);
    auto frame_at = [&](int64_t n, float* lr) {
        if (n < 0 || n >= int64_t(queued_end_)) lr[0] = lr[1] = 0;
        else recent_frame(mem, mem_size, uint64_t(n), lr);
    };
    for (uint32_t i = 0; i < frames; ++i) {
        retire_played();
        if (packets_.empty() || pos_ >= double(queued_end_)) {
            // Out of data: stopped after a discontinuity, else starved (waiting, silent).
            if (discontinuity_) playing_ = false, discontinuity_ = false;
            return;
        }
        const uint64_t f0 = uint64_t(pos_);
        float s[2];
        resample(f0, pos_ - double(f0), frame_at, s);
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t d = 0; d < dst; ++d) out[dst * i + d] += s[c] * to[c][d];
        pos_ += step;
    }
    retire_played();
}

// ---------------------------------------------------------------- saturation and the limiter
void saturate(Bins& bins, uint32_t first_bin, uint32_t count) {
    constexpr float kTop = 1.0f - 1.0f / 8388608.0f;
    for (uint32_t b = first_bin; b < first_bin + count && b < kMixBinCount; ++b) {
        float* x = bins.bin(b);
        for (uint32_t i = 0; i < bins.frames; ++i) x[i] = std::isfinite(x[i]) ? std::clamp(x[i], -1.0f, kTop) : 0.0f;  // never pass on a NaN
    }
}

void Limiter::process(float* io, uint32_t frames) {
    // Each frame waits kLookahead frames in the ring; slot pos_ holds the
    // one going out now, then pos_ + 1 … the ones after it, k frames from
    // going out. For each frame needing less gain than now, the straight
    // line from here that reaches its need when it goes out; the gain takes
    // the steepest of them (so every frame gets at most what it needs), and
    // otherwise recovers by a slow one-pole, never above what any frame in
    // the window needs, snapping to unity when within 10^−5.
    constexpr double kRelease = 0.99958;  // e^(−1/(0.05 · 48000))
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = io[2 * i], r = io[2 * i + 1];
        const float peak = std::max(std::fabs(l), std::fabs(r));
        const float incoming = peak > 1.0f ? 1.0f / peak : 1.0f;  // goes out kLookahead frames from now
        double next = incoming < gain_ ? gain_ - (gain_ - incoming) / double(kLookahead + 1) : gain_;
        double lowest = incoming;
        for (uint32_t k = 0; k < kLookahead; ++k) {
            const float n = need_[(pos_ + k) % kLookahead];
            lowest = std::min(lowest, double(n));
            if (n < gain_) next = std::min(next, gain_ - (gain_ - n) / double(k + 1));
        }
        if (next < gain_) {
            gain_ = next;
        } else {
            gain_ = std::min(lowest, 1.0 - (1.0 - gain_) * kRelease);
            if (gain_ > 0.99999 && lowest >= 1.0) gain_ = 1.0;
        }
        const float out_l = delay_[2 * pos_], out_r = delay_[2 * pos_ + 1];
        delay_[2 * pos_] = l, delay_[2 * pos_ + 1] = r;
        need_[pos_] = incoming;
        pos_ = (pos_ + 1) % kLookahead;
        io[2 * i] = gain_ >= 1.0 ? out_l : float(out_l * gain_);
        io[2 * i + 1] = gain_ >= 1.0 ? out_r : float(out_r * gain_);
    }
}

// ---------------------------------------------------------------- WAV output
namespace {
void le16(std::FILE* f, uint32_t v) {
    const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
void le32(std::FILE* f, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
}  // namespace

WavWriter::WavWriter(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (file_) finish();
}

WavWriter::~WavWriter() {
    if (file_) {
        finish();
        std::fclose(file_);
    }
}

// The header, with the sizes so far; the file stays a valid WAV at every point.
void WavWriter::finish() {
    const long here = std::ftell(file_);
    std::fseek(file_, 0, SEEK_SET);
    const uint32_t data = uint32_t(frames_ * 4);
    std::fwrite("RIFF", 1, 4, file_);
    le32(file_, 36 + data);
    std::fwrite("WAVEfmt ", 1, 8, file_);
    le32(file_, 16);
    le16(file_, 1);
    le16(file_, 2);
    le32(file_, kOutputRate);
    le32(file_, kOutputRate * 4);
    le16(file_, 4);
    le16(file_, 16);
    std::fwrite("data", 1, 4, file_);
    le32(file_, data);
    if (here > 44) std::fseek(file_, here, SEEK_SET);
}

void WavWriter::write(const float* stereo, uint32_t frames) {
    if (!file_) return;
    std::vector<uint8_t> buf(size_t(frames) * 4);
    for (uint32_t i = 0; i < frames * 2; ++i) {
        const int32_t v = int32_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
        buf[2 * i] = uint8_t(v);
        buf[2 * i + 1] = uint8_t(uint32_t(v) >> 8);
    }
    std::fwrite(buf.data(), 1, buf.size(), file_);
    frames_ += frames;
    if (frames_ % (kOutputRate / 2) < frames) {  // every half second, keep the header current
        finish();
        std::fflush(file_);
    }
}

}  // namespace qilin::audio
