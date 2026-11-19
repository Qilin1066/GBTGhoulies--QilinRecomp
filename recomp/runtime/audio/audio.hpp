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

// The APU's attenuation (1/64 dB units, 12 bits) as the gain it applies,
// in 1/16384 (measured on a retail console, xemu pull request 3105):
// straight lines between whole decibels through a table of
// round(4095 · 10^(−k/20)) rounded down (in 1/4096), and from 48 dB a
// straight line to 0 at 63 dB, 0xFC0 and above silent; 0 is unity.
uint32_t attenuation_gain(uint32_t attenuation);
// A route's attenuation from millibels, as DirectSound converts them.
uint32_t attenuation_for(int64_t millibels);

// A head-related filter tap as the hardware reads it: sign and magnitude,
// a 3-bit exponent and a 4-bit mantissa, (16 + m) · 2^(e − 1) / 2048, or
// m / 2048 with exponent 0 (0x40 is 1/16, 0x7F 31/32). Applied as written.
float hrtf_tap(uint8_t t);

// The amplitude envelope (DSENVELOPEDESC with DSEG_AMPLITUDE): delay,
// attack, hold, decay and release in units of 512 frames (the APU's
// envelope counter: 16 ticks of its 1500 Hz clock), sustain as 0-255. The
// attack rises linearly, the decay falls exponentially to the sustain
// level, the release linearly from where it is to silence. mode is where a
// voice starts it: 0 off, 1 the delay, 2 the attack, 3 the hold.
struct Envelope {
    uint32_t mode = 0, delay = 0, attack = 0, hold = 0, decay = 0, release = 0, sustain = 255;
};

// The voice filter (DSFILTERDESC: dwMode, dwQCoefficient, adwCoefficients),
// which DirectSound (CMcpxVoiceClient's SetFilter, 0x1CC93A) packs into
// the voice's registers: FCA = coefficient 1 << 16 | coefficient 0, FCB =
// 3 << 16 | 2, the mode into MISC bits 17:16 and Q (& 7) into 20:18. The
// hardware's filters are a state-variable filter used as the DLS2
// low-pass (FC0 its frequency, FC1 its damping) or as a peaking EQ (bits
// 15:0 the frequency, 31:16 the signed gain), and for 3D voices a pair of
// one-pole I3DL2 filters (Voice::process says which, by mode and voice).
struct Filter {
    uint32_t mode = 0, q = 0;
    uint32_t coefficients[4] = {};
    uint32_t fca() const { return coefficients[1] << 16 | (coefficients[0] & 0xFFFF); }
    uint32_t fcb() const { return coefficients[3] << 16 | (coefficients[2] & 0xFFFF); }
};

// The filters' state-variable core as the hardware runs it, at 48 kHz:
// each step (low, high, band) rounded to 1/16 of a 24-bit sample, halves up.
struct Svf {
    double low = 0, band = 0;
    // The frequency coefficient from a 16-bit FC: 2^(FC/4096) to a 1024th
    // of an octave, then to 1/65536, at most 0xFFFF/65536.
    static double frequency(uint32_t fc);
    double step(double x, double f, double damping);  // returns the high-pass; low and band updated
};

// A 3D voice's processing (DirectSound's 3D voice data, as the original
// programs it): a volume offset per mix bin, a pitch offset (Doppler),
// and the head-related filter, 31 taps a ear and the interaural delay.
struct Voice3d {
    bool active = false;
    int32_t bin_offset[kMixBinCount] = {};  // millibels, added to each bin's volume
    int32_t pitch_offset = 0;
    bool hrtf = false;
    uint8_t taps[2][31] = {};  // left, right: the hardware's minifloats (hrtf_tap)
    int32_t itd = 0;           // frames the far ear lags
    // The I3DL2 filters' coefficients (DS3DVOICEDATA's, bit 7), the voice's
    // FCB in place of SetFilter's: the direct path's in bits 15:0, the
    // room's in 31:16.
    bool fcb_set = false;
    uint32_t fcb = 0;
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
    Svf dls2_[2], peq_[2];          // per channel
    double i3dl2_[2] = {};          // a 3D voice's direct and room one-pole states
    std::vector<float> room_;       // process(): a 3D voice's room path (routes 5 and 7)
    std::vector<float> route_gain_; // mix(): each route's gain at the end of the last block
    bool fresh_ = true;             // a new voice fades in over its first frame
    std::vector<float> scratch_;     // process(): channel rows
    std::vector<float> hrtf_history_;  // the last 30 + itd frames of a 3D voice's signal, for its filter
    float hrtf_energy_[2] = {};
    std::vector<float> hrtf_out_;      // process(): the left and right ears' rows

    void decode_frame(const uint8_t* mem, size_t mem_size, uint64_t frame, float* lr);
    // The last frames decoded, by index (direct-mapped): the resampler
    // reads 8 around each position, and a stream's are still wanted after
    // their packet has been retired. A buffer's are forgotten each block
    // (the game may rewrite its data), a stream's when it restarts.
    struct RecentFrame {
        uint64_t frame = UINT64_MAX;
        float lr[2] = {};
    };
    std::array<RecentFrame, 16> recent_;
    void recent_frame(const uint8_t* mem, size_t mem_size, uint64_t frame, float* lr);
    void forget_recent() { for (RecentFrame& r : recent_) r.frame = UINT64_MAX; }
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
