// The audio engine: voices playing PCM or Xbox ADPCM sample data from guest
// memory, mixed to 48 kHz stereo (docs/audio.md). The DirectSound
// replacement (recomp/runtime/host/dsound.cpp) drives it; nothing here knows
// about DirectSound's objects or the guest's calling conventions.
//
// Units are the Xbox's: volumes in hundredths of a decibel (0 is full,
// -10000 silence), pitch in 1/4096 of an octave relative to 48 kHz.
#pragma once

#include <array>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace qilin::audio {

inline constexpr uint32_t kOutputRate = 48000;  // the APU's mixing rate
inline constexpr int32_t kVolumeMin = -10000;

struct Format {
    enum class Codec { Pcm, XboxAdpcm } codec = Codec::Pcm;
    uint32_t channels = 1;  // 1 or 2
    uint32_t rate = 48000;  // samples per second, per channel
    uint32_t bits = 16;     // PCM: 8 (unsigned) or 16 (signed); ADPCM: 4
    // Bytes per frame (PCM) or per block of 64 frames (ADPCM: 36 per channel).
    uint32_t block_bytes() const;
    uint32_t frames_per_block() const { return codec == Codec::XboxAdpcm ? 64 : 1; }
    uint64_t bytes_to_frames(uint64_t bytes) const { return bytes / block_bytes() * frames_per_block(); }
    uint64_t frames_to_bytes(uint64_t frames) const { return frames / frames_per_block() * block_bytes(); }
};

// The Xbox's mix bins: the five speakers and LFE, then effect sends.
enum MixBin : uint32_t { kFrontLeft = 0, kFrontRight = 1, kFrontCenter = 2, kLowFrequency = 3, kBackLeft = 4, kBackRight = 5 };
struct MixBinVolume {
    uint32_t bin;
    int32_t volume;
};

// The pitch value that plays a sample rate at its own speed.
int32_t pitch_for_rate(uint32_t rate);

class Voice {
public:
    // Sample data in guest memory: the mixer reads it as it plays.
    void set_data(uint32_t guest_addr, uint32_t bytes);
    void set_format(const Format& f);
    void set_play_region(uint32_t start_bytes, uint32_t length_bytes);  // length 0: to the end
    void set_loop_region(uint32_t start_bytes, uint32_t length_bytes);  // length 0: the whole play region
    void set_position(uint32_t bytes);                                  // from the buffer's start
    uint32_t position_bytes() const;
    void play(bool looping);
    void stop();
    bool playing() const { return playing_; }
    bool looping() const { return looping_; }

    int32_t volume = 0, pitch = 0, headroom = 0;
    std::vector<MixBinVolume> mix_bins;  // which bins each channel feeds, in channel order

    const Format& format() const { return format_; }

    // Mixes `frames` output frames into out (interleaved stereo), reading
    // guest memory mem[0, mem_size). Stops the voice at the end of a
    // non-looping play region. A submix voice plays its input instead.
    void mix(const uint8_t* mem, size_t mem_size, float* out, uint32_t frames);

    // Submixing (DirectSound's DSBCAPS_MIXIN): a submix voice has no sample
    // data; other voices mix into its input, one float per channel and
    // frame at the output rate, and it plays that like a voice's data, with
    // its own volume and mix bins (its pitch doesn't apply). The input is
    // cleared before each block is mixed.
    void make_submix(uint32_t channels);
    bool submix() const { return submix_; }
    void clear_input(uint32_t frames) { input_.assign(size_t(frames) * format_.channels, 0.0f); }
    // Mixes `frames` frames of this voice into a submix voice's input: channel
    // k into the submix's channel k (a mono submix takes the sum), at this
    // voice's volume.
    void mix_into(const uint8_t* mem, size_t mem_size, Voice& submix, uint32_t frames);

    // Streaming (DirectSound streams): the data comes as packets in guest
    // memory, played one after another as one continuous signal (the
    // interpolation runs across their edges). With none left the voice is
    // starved: it plays silence and waits, and the next packet carries on.
    // After a discontinuity it stops instead, when the packets run out,
    // until more come. Each packet is a whole number of the format's blocks.
    void make_stream();
    bool stream() const { return stream_; }
    void queue_packet(uint32_t guest_addr, uint32_t bytes, uint32_t id);
    struct Done {
        uint32_t id, bytes;  // the packet, and how much of it played
    };
    std::vector<Done> take_completed();  // packets played to their end since the last call, oldest first
    std::vector<Done> flush();           // drops every packet not yet played (stopping the voice), oldest first
    void discontinuity() { discontinuity_ = true; }
    void pause(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    bool starved() const { return stream_ && playing_ && packets_.empty(); }
    size_t packets_queued() const { return packets_.size(); }

private:
    // Renders frames into out (`dst` channels interleaved), channel c of the
    // voice scaled by to[c][d] into channel d.
    void render(const uint8_t* mem, size_t mem_size, float* out, uint32_t dst, const float to[2][2], uint32_t frames);
    void decode_frame(const uint8_t* mem, size_t mem_size, uint64_t frame, float* lr);
    uint64_t region_end() const;

    Format format_;
    uint32_t data_ = 0, bytes_ = 0;
    uint64_t play_start_ = 0, play_len_ = 0, loop_start_ = 0, loop_len_ = 0;  // in frames
    double pos_ = 0;  // in frames from the buffer's start
    bool playing_ = false, looping_ = false;
    bool submix_ = false;
    std::vector<float> input_;
    // The ADPCM block last decoded, per channel.
    uint64_t cached_block_ = UINT64_MAX;
    std::array<std::array<int16_t, 64>, 2> block_{};
    // Streaming: the packets queued, each covering frames [first, first + frames).
    struct Packet {
        uint32_t addr, bytes, id;
        uint64_t first, frames;
    };
    bool stream_ = false, discontinuity_ = false, paused_ = false;
    std::deque<Packet> packets_;
    uint64_t queued_end_ = 0;  // the frame after the last one queued
    std::vector<Done> completed_;
    void render_stream(const uint8_t* mem, size_t mem_size, float* out, uint32_t dst, const float to[2][2], uint32_t frames);
    void retire_played();
};

// Xbox ADPCM: decodes one block (36 bytes per channel, channel headers then
// interleaved 4-byte groups) into 64 samples per channel.
void decode_xbox_adpcm_block(const uint8_t* block, uint32_t channels, int16_t out[][64]);

// Writes 16-bit stereo PCM at kOutputRate as a WAV file, growing as frames arrive.
class WavWriter {
public:
    explicit WavWriter(const std::string& path);
    ~WavWriter();
    bool ok() const { return file_ != nullptr; }
    void write(const float* stereo, uint32_t frames);

private:
    void finish();
    std::FILE* file_ = nullptr;
    uint64_t frames_ = 0;
};

}  // namespace qilin::audio
