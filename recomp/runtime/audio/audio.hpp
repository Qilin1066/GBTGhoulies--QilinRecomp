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

// The Xbox's mix bins: the five speakers and LFE, the crosstalk
// canceller's four inputs (3D voices' binaural signal), the I3DL2 reverb's
// input, then the other effect sends.
enum MixBin : uint32_t {
    kFrontLeft = 0, kFrontRight = 1, kFrontCenter = 2, kLowFrequency = 3, kBackLeft = 4, kBackRight = 5,
    kXtlkFrontLeft = 6, kXtlkFrontRight = 7, kXtlkBackLeft = 8, kXtlkBackRight = 9, kI3dl2 = 10, kFxSend0 = 11,
};
inline constexpr uint32_t kMixBinCount = 32;
struct MixBinVolume {
    uint32_t bin;
    int32_t volume;
};

// One block of the mix: kMixBinCount rows of `frames` floats.
struct Bins {
    std::vector<float> data;
    uint32_t frames = 0;
    void clear(uint32_t n) { frames = n, data.assign(size_t(kMixBinCount) * n, 0.0f); }
    float* bin(uint32_t b) { return data.data() + size_t(b) * frames; }
};

// dB/100 (millibels) to a linear gain; kVolumeMin and below is silence.
float gain(int32_t millibels);

// The amplitude envelope (DSENVELOPEDESC with DSEG_AMPLITUDE): delay,
// attack, hold, decay and release in units of 512 frames (the APU's
// envelope counter: 16 ticks of its 1500 Hz clock), sustain as 0-255. The
// attack rises linearly, the decay falls exponentially to the sustain
// level, the release linearly from where it is to silence. mode is where a
// voice starts it: 0 off, 1 the delay, 2 the attack, 3 the hold.
struct Envelope {
    uint32_t mode = 0, delay = 0, attack = 0, hold = 0, decay = 0, release = 0, sustain = 255;
};

// The voice filter (DSFILTERDESC): mode 0 bypass, 1 DLS2 low-pass, 2
// parametric EQ, 3 both; the EQ's Q; two coefficients a channel, as the
// hardware takes them. The DLS2 cutoff is 4096·log2(2·sin(π·f/48000)) as a signed
// 16-bit value; its resonance is Q = 10^(−R/20), R in dB to 22.5, as a
// 16-bit fraction.
struct Filter {
    uint32_t mode = 0, q = 0;
    uint32_t coefficients[4] = {};
};

// A 3D voice's processing (DirectSound's 3D voice data, as the original
// programs it): a volume offset per mix bin, a pitch offset (Doppler),
// and the head-related filter, 31 taps a ear and the interaural delay.
struct Voice3d {
    bool active = false;
    int32_t bin_offset[kMixBinCount] = {};  // millibels, added to each bin's volume
    int32_t pitch_offset = 0;
    bool hrtf = false;
    int8_t taps[2][31] = {};  // left, right
    int32_t itd = 0;          // frames the far ear lags
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
    uint32_t data_address() const { return data_; }
    uint32_t data_bytes() const { return bytes_; }
    void play(bool looping);
    void stop();
    bool playing() const { return playing_; }
    bool looping() const { return looping_; }

    int32_t volume = 0, pitch = 0, headroom = 0;
    std::vector<MixBinVolume> mix_bins;  // which bins each channel feeds, in channel order
    Voice3d three_d;

    const Format& format() const { return format_; }

    // The envelope, applied from the next Play (or now, if one is playing
    // and has none running); release() starts its release phase, after which
    // the voice stops (at once without an envelope).
    void set_envelope(const Envelope& e);
    void release();
    void set_filter(const Filter& f);

    // Mixes `frames` frames into the mix bins, reading guest memory
    // mem[0, mem_size): each channel through the envelope and filter, into
    // the bins it feeds at their volumes (a 3D voice's left- and right-side
    // bins through its head-related filter). Stops the voice at the end of a
    // non-looping play region. A submix voice plays its input instead.
    // A voice with an output buffer (SetOutputBuffer) also feeds that
    // submix's input, channel for channel, at its volume: its own bins
    // carry its effect sends (the reverb), the submix its dry signal.
    void mix(const uint8_t* mem, size_t mem_size, Bins& out, uint32_t frames, Voice* submix = nullptr);

    // Submixing (DirectSound's DSBCAPS_MIXIN): a submix voice has no sample
    // data; other voices mix into its input, one float per channel and
    // frame at the output rate, and it plays that like a voice's data, with
    // its own volume and mix bins (its pitch doesn't apply). The input is
    // cleared before each block is mixed. It plays from the start: XACT
    // never calls Play on its submixes (its 3D sounds' voices), and they
    // sound on the console.
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
    // The voice's own signal for the block, one row a channel, through the
    // envelope and the filter, in scratch_; false if it's silent.
    bool process(const uint8_t* mem, size_t mem_size, uint32_t frames);
    void feed(Voice* submix, uint32_t frames);  // the processed signal into a submix's input
    float envelope_step();  // the envelope's level for one frame, advancing it
    void start_envelope();

    Envelope eg_;
    enum class Stage : uint8_t { Off, Delay, Attack, Hold, Decay, Sustain, Release } stage_ = Stage::Off;
    uint64_t stage_frames_ = 0;  // frames into the stage
    float level_ = 1.0f, release_from_ = 1.0f;
    Filter filter_;
    float filter_state_[2][2] = {};  // per channel: the DLS2 filter's last two outputs
    std::vector<float> scratch_;     // process(): channel rows
    std::vector<float> hrtf_history_;  // the last 30 + itd frames of a 3D voice's signal, for its filter
    float hrtf_energy_[2] = {};
    std::vector<float> hrtf_out_;      // process(): the left and right ears' rows

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

// The I3DL2 environmental reverb (the APU's DSP runs it from the game's
// effects image; here natively, with the same parameters): DSI3DL2LISTENER's
// room and high-frequency room levels, decay time and its HF ratio,
// reflections and their delay, reverb and its delay, diffusion, density
// and the HF reference. Its input is the I3DL2 mix bin; its output four
// decorrelated channels into the front and back speaker bins.
struct I3dl2Params {
    int32_t room = -10000, room_hf = 0;
    float room_rolloff = 0, decay_time = 1.49f, decay_hf_ratio = 0.83f;
    int32_t reflections = -2602;
    float reflections_delay = 0.007f;
    int32_t reverb = 200;
    float reverb_delay = 0.011f, diffusion = 100, density = 100, hf_reference = 5000;
};
class I3dl2Reverb {
public:
    I3dl2Reverb();
    void set(const I3dl2Params& p);
    bool on() const { return on_; }
    // Reads bins.bin(kI3dl2), adds into the four speaker bins.
    void process(Bins& bins);

private:
    static constexpr uint32_t kLines = 8;
    I3dl2Params p_;
    bool on_ = false;
    float in_gain_ = 0, in_lp_ = 0, in_lp_state_ = 0;
    std::vector<float> pre_;  // the input's delay line (reflections and the late reverb read it)
    uint32_t pre_pos_ = 0;
    uint32_t reflection_taps_[4] = {}, late_delay_ = 0;
    float reflections_gain_ = 0, late_gain_ = 0;
    struct Allpass { std::vector<float> buf; uint32_t pos = 0; float g = 0; float run(float x); };
    Allpass diffusers_[2];
    struct Line { std::vector<float> buf; uint32_t pos = 0; float gain = 0, lp = 0, state = 0; };
    Line lines_[kLines];
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
