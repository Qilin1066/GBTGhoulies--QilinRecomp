// Voices: decoding PCM and Xbox ADPCM, resampling, gain and mix bins.
#include <algorithm>
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

// dB/100 to a linear gain.
float gain(int32_t millibels) {
    if (millibels <= kVolumeMin) return 0.0f;
    return float(std::pow(10.0, double(std::min(millibels, 0)) / 2000.0));
}

}  // namespace

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
    if (!playing_ && (pos_ < double(play_start_) || pos_ >= double(region_end()))) pos_ = double(play_start_);
    playing_ = bytes_ != 0 || submix_;
}

void Voice::stop() { playing_ = false; }

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

void Voice::mix(const uint8_t* mem, size_t mem_size, float* out, uint32_t frames) {
    if (!playing_ || (bytes_ == 0 && !submix_ && !stream_)) return;
    // Each channel's gain into the left and right outputs: the mix bins
    // folded down to stereo. Bins after BackRight are effect sends, which
    // aren't modelled yet.
    float to[2][2] = {};
    const float base = gain(volume);
    const uint32_t ch = format_.channels;
    std::vector<MixBinVolume> bins = mix_bins;
    if (bins.empty()) bins = {{kFrontLeft, 0}, {kFrontRight, 0}};
    for (size_t k = 0; k < bins.size(); ++k) {
        const uint32_t c = ch == 1 ? 0 : uint32_t(k % ch);
        const float g = base * gain(bins[k].volume);
        switch (bins[k].bin) {
            case kFrontLeft: case kBackLeft: to[c][0] += g; break;
            case kFrontRight: case kBackRight: to[c][1] += g; break;
            case kFrontCenter: to[c][0] += g * 0.70710678f; to[c][1] += g * 0.70710678f; break;
            default: break;  // LFE and effect sends
        }
    }
    render(mem, mem_size, out, 2, to, frames);
}

void Voice::make_submix(uint32_t channels) {
    submix_ = true;
    format_ = Format{};
    format_.channels = channels;
    format_.rate = kOutputRate;
}

void Voice::mix_into(const uint8_t* mem, size_t mem_size, Voice& submix, uint32_t frames) {
    if (!playing_ || (bytes_ == 0 && !stream_)) return;
    const float g = gain(volume);
    float to[2][2] = {};
    const uint32_t dst = submix.format_.channels;
    for (uint32_t c = 0; c < format_.channels; ++c) {
        if (dst == 1) to[c][0] = g;
        else if (format_.channels == 1) to[0][0] = to[0][1] = g;
        else to[c][c] = g;
    }
    if (submix.input_.size() < size_t(frames) * dst) submix.input_.resize(size_t(frames) * dst, 0.0f);
    render(mem, mem_size, submix.input_.data(), dst, to, frames);
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
    const double step = std::pow(2.0, double(pitch) / 4096.0);
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
        // Linear interpolation between this frame and the next (which wraps on a loop).
        const uint64_t f0 = uint64_t(pos_);
        uint64_t f1 = f0 + 1;
        if (f1 >= limit) f1 = looping_ ? loop_start : f0;
        const float t = float(pos_ - double(f0));
        float a[2], b[2];
        decode_frame(mem, mem_size, f0, a);
        decode_frame(mem, mem_size, f1, b);
        const float s[2] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t d = 0; d < dst; ++d) out[dst * i + d] += s[c] * to[c][d];
        pos_ += step;
    }
}

// ---------------------------------------------------------------- streaming
void Voice::make_stream() {
    stream_ = true;
    packets_.clear();
    completed_.clear();
    queued_end_ = 0;
    pos_ = 0;
}

void Voice::queue_packet(uint32_t guest_addr, uint32_t bytes, uint32_t id) {
    const uint64_t frames = format_.bytes_to_frames(bytes);
    packets_.push_back({guest_addr, bytes, id, queued_end_, frames});
    // A voice stopped (after a discontinuity, or never started) starts again
    // at the new packet: the frames since the last one are silence, not data.
    if (!playing_) pos_ = double(queued_end_);
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
    const double step = std::pow(2.0, double(pitch) / 4096.0);
    for (uint32_t i = 0; i < frames; ++i) {
        retire_played();
        if (packets_.empty() || pos_ >= double(queued_end_)) {
            // Out of data: stopped after a discontinuity, else starved (waiting, silent).
            if (discontinuity_) playing_ = false, discontinuity_ = false;
            return;
        }
        const uint64_t f0 = uint64_t(pos_);
        const uint64_t f1 = f0 + 1 < queued_end_ ? f0 + 1 : f0;
        const float t = float(pos_ - double(f0));
        float a[2], b[2];
        decode_frame(mem, mem_size, f0, a);
        decode_frame(mem, mem_size, f1, b);
        const float s[2] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t d = 0; d < dst; ++d) out[dst * i + d] += s[c] * to[c][d];
        pos_ += step;
    }
    retire_played();
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
