// Audio engine tests: voices over a fake guest memory, no device needed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio.hpp"

using namespace qilin;

namespace {
int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
}  // namespace

int main() {
    std::vector<uint8_t> mem(0x10000, 0);
    // A voice's front left and right bins for n frames, interleaved.
    auto stereo = [&](audio::Voice& v, uint32_t n) {
        audio::Bins b;
        b.clear(n);
        v.mix(mem.data(), mem.size(), b, n);
        std::vector<float> o(std::max<size_t>(16, 2 * size_t(n)), 0.0f);
        for (uint32_t i = 0; i < n; ++i) o[2 * i] = b.bin(audio::kFrontLeft)[i], o[2 * i + 1] = b.bin(audio::kFrontRight)[i];
        return o;
    };

    // A 16-bit mono PCM voice at 48 kHz: plays sample for sample to both
    // sides (default mix bins), then stops at the end of its data.
    const int16_t pcm[4] = {16384, -16384, 8192, 0};
    std::memcpy(mem.data() + 0x100, pcm, sizeof pcm);
    audio::Voice v;
    audio::Format f;
    v.set_format(f);
    CHECK(v.pitch == 0);
    v.set_data(0x100, sizeof pcm);
    v.play(false);
    std::vector<float> out(16, 0.0f);
    out = stereo(v, 8);
    CHECK(near(out[0], 0.5f) && near(out[1], 0.5f) && near(out[2], -0.5f) && near(out[4], 0.25f));
    CHECK(!v.playing() && near(out[8], 0.0f));

    // Volume in hundredths of a decibel: -600 is half amplitude (6.02 dB, near enough).
    out.assign(16, 0.0f);
    v.volume = -602;
    v.play(false);
    v.set_position(0);
    v.play(false);
    out = stereo(v, 1);
    CHECK(near(out[0], 0.25f, 1e-3f));
    v.volume = 0;

    // A loop keeps it playing past the end; a 24 kHz voice plays at half speed.
    out.assign(16, 0.0f);
    v.set_position(0);
    v.play(true);
    out = stereo(v, 8);
    CHECK(v.playing() && v.looping() && near(out[8], 0.5f));
    v.stop();
    f.rate = 24000;
    v.set_format(f);
    CHECK(v.pitch == -4096);
    v.set_data(0x100, sizeof pcm);
    out.assign(16, 0.0f);
    v.play(false);
    out = stereo(v, 3);
    CHECK(near(out[0], 0.5f) && near(out[2], 0.0f) && near(out[4], -0.5f));  // halfway between samples 0 and 1

    // Mix bins: a voice sent only to the front right is silent on the left.
    f.rate = 48000;
    v.set_format(f);
    v.set_data(0x100, sizeof pcm);
    v.mix_bins = {{audio::kFrontRight, 0}};
    out.assign(16, 0.0f);
    v.play(false);
    out = stereo(v, 1);
    CHECK(near(out[0], 0.0f) && near(out[1], 0.5f));

    // Xbox ADPCM: the header seeds the predictor; nibble 4 adds the step and
    // moves the index up 2 (steps 7, 9, 11 …).
    uint8_t block[36] = {};
    block[2] = 0;  // predictor 0, index 0
    std::memset(block + 4, 0x44, 32);
    int16_t dec[2][64];
    audio::decode_xbox_adpcm_block(block, 1, dec);
    CHECK(dec[0][0] == 7 && dec[0][1] == 17 && dec[0][2] == 29);
    CHECK((audio::Format{audio::Format::Codec::XboxAdpcm, 2, 44100, 4}.block_bytes() == 72));
    CHECK((audio::Format{audio::Format::Codec::XboxAdpcm, 1, 44100, 4}.bytes_to_frames(72) == 128));

    // Submixing: a mono voice into a stereo submix at -6.02 dB (half), and the
    // submix to the speakers at full volume, its channels to left and right.
    {
        audio::Voice src, sub;
        src.set_format(audio::Format{});
        src.set_data(0x100, sizeof pcm);
        src.volume = -602;
        src.play(false);
        sub.make_submix(2);
        sub.mix_bins = {{audio::kFrontLeft, 0}, {audio::kFrontRight, 0}};
        sub.play(true);
        CHECK(sub.submix() && sub.playing());
        sub.clear_input(4);
        src.mix_into(mem.data(), mem.size(), sub, 4);
        std::vector<float> o(8, 0.0f);
        o = stereo(sub, 4);
        CHECK(near(o[0], 0.25f, 1e-3f) && near(o[1], 0.25f, 1e-3f) && near(o[2], -0.25f, 1e-3f) && near(o[4], 0.125f, 1e-3f));
        sub.clear_input(4);  // nothing fed: silence
        std::vector<float> o2(8, 0.0f);
        o2 = stereo(sub, 4);
        CHECK(near(o2[0], 0.0f) && sub.playing());
    }

    // A stream: packets play back to back as one signal, complete in order,
    // starve (silence, still playing) when they run out and carry on with the
    // next; a discontinuity stops it at the end; a flush drops what's queued.
    {
        const int16_t a[3] = {16384, 16384, 16384}, b[2] = {-16384, -16384};
        std::memcpy(mem.data() + 0x400, a, sizeof a);
        std::memcpy(mem.data() + 0x500, b, sizeof b);
        audio::Voice s;
        s.set_format(audio::Format{});
        s.make_stream();
        CHECK(!s.playing());
        s.queue_packet(0x400, sizeof a, 1);
        s.queue_packet(0x500, sizeof b, 2);
        std::vector<float> o(16, 0.0f);
        o = stereo(s, 4);
        CHECK(near(o[0], 0.5f) && near(o[4], 0.5f) && near(o[6], -0.5f));  // the second packet follows at once
        auto done = s.take_completed();
        CHECK(done.size() == 1 && done[0].id == 1 && done[0].bytes == sizeof a);
        o.assign(16, 0.0f);
        o = stereo(s, 4);  // one frame left, then starved
        done = s.take_completed();
        CHECK(near(o[0], -0.5f) && near(o[2], 0.0f) && done.size() == 1 && done[0].id == 2);
        CHECK(s.playing() && s.starved());
        s.queue_packet(0x400, sizeof a, 3);  // carries on
        o.assign(16, 0.0f);
        o = stereo(s, 2);
        CHECK(near(o[0], 0.5f) && !s.starved());
        s.discontinuity();
        o.assign(16, 0.0f);
        o = stereo(s, 4);
        CHECK(!s.playing() && s.take_completed().size() == 1);
        s.queue_packet(0x400, sizeof a, 4);
        s.queue_packet(0x500, sizeof b, 5);
        o.assign(16, 0.0f);
        o = stereo(s, 1);
        const auto flushed = s.flush();
        CHECK(flushed.size() == 2 && flushed[0].id == 4 && flushed[0].bytes == 2 && flushed[1].bytes == 0 && !s.playing());
    }

    // The envelope: release units of 512 frames, linear to silence, then the
    // voice stops; attack rises linearly from the start.
    {
        std::vector<int16_t> dc(4096, 16384);
        std::memcpy(mem.data() + 0x1000, dc.data(), dc.size() * 2);
        audio::Voice e;
        e.set_format(audio::Format{});
        e.set_data(0x1000, uint32_t(dc.size() * 2));
        audio::Envelope env;
        env.mode = 1, env.attack = 1, env.release = 1, env.sustain = 255;
        e.set_envelope(env);
        e.play(true);
        auto o = stereo(e, 512);
        CHECK(near(o[0], 0.5f / 512.0f, 1e-4f) && near(o[2 * 255], 0.25f, 1e-3f) && near(o[2 * 511], 0.5f, 1e-3f));
        o = stereo(e, 8);
        CHECK(near(o[0], 0.5f, 1e-3f));  // hold, decay to the full sustain
        e.release();
        o = stereo(e, 256);
        CHECK(e.playing() && near(o[2 * 255], 0.25f, 2e-3f));
        o = stereo(e, 300);
        CHECK(!e.playing() && near(o[2 * 299], 0.0f));
        // Without an envelope a release is a stop.
        audio::Voice q;
        q.set_format(audio::Format{});
        q.set_data(0x1000, uint32_t(dc.size() * 2));
        q.play(true);
        q.release();
        CHECK(!q.playing());
    }

    // The DLS2 filter: a low cutoff passes DC (unity) and stops a tone at
    // the Nyquist frequency; a cutoff code of 0 (8 kHz) less so.
    {
        std::vector<int16_t> alt(2048);
        for (size_t i = 0; i < alt.size(); ++i) alt[i] = i & 1 ? -16384 : 16384;
        std::memcpy(mem.data() + 0x3000, alt.data(), alt.size() * 2);
        audio::Voice lp;
        lp.set_format(audio::Format{});
        lp.set_data(0x3000, uint32_t(alt.size() * 2));
        audio::Filter flt;
        flt.mode = 1;
        flt.coefficients[0] = uint32_t(uint16_t(int16_t(std::lround(4096.0 * std::log2(2.0 * std::sin(3.14159265 * 500.0 / 48000.0))))));
        flt.coefficients[1] = 0xFFFF;  // no resonance
        lp.set_filter(flt);
        lp.play(true);
        auto o = stereo(lp, 1024);
        float peak = 0;
        for (uint32_t i = 512; i < 1024; ++i) peak = std::max(peak, std::fabs(o[2 * i]));
        CHECK(peak < 0.005f);
    }

    // A 3D voice: per-bin offsets (the back bins 20 dB down here), the
    // head-related filter's left taps into the left-side bins, right into
    // the right, and the Doppler pitch offset.
    {
        audio::Voice d;
        d.set_format(audio::Format{});
        d.set_data(0x100, sizeof pcm);
        d.mix_bins = {{audio::kXtlkFrontLeft, 0}, {audio::kXtlkFrontRight, 0}, {audio::kXtlkBackLeft, 0}, {audio::kI3dl2, 0}};
        d.three_d.active = true;
        d.three_d.hrtf = true;
        d.three_d.taps[0][0] = 64;  // left: an eighth (taps count 1/512), at once
        d.three_d.taps[1][2] = 127; // right: louder, two frames later
        d.three_d.bin_offset[audio::kXtlkBackLeft] = -2000;
        d.play(false);
        audio::Bins b;
        b.clear(4);
        d.mix(mem.data(), mem.size(), b, 4);
        CHECK(near(b.bin(audio::kXtlkFrontLeft)[0], 0.0625f, 1e-3f));
        CHECK(near(b.bin(audio::kXtlkFrontRight)[0], 0.0f) && near(b.bin(audio::kXtlkFrontRight)[2], 0.5f * 127 / 512, 1e-3f));
        CHECK(near(b.bin(audio::kXtlkBackLeft)[0], 0.00625f, 1e-4f));
        CHECK(near(b.bin(audio::kI3dl2)[0], 0.5f) && near(b.bin(audio::kI3dl2)[1], -0.5f));  // the send is the dry signal
    }

    // The reverb: off at the room's minimum; on, an impulse into the I3DL2
    // bin comes out of all four speaker bins after the reflections delay,
    // and decays.
    {
        audio::I3dl2Reverb r;
        audio::I3dl2Params p;
        r.set(p);
        CHECK(!r.on());
        p.room = -1000, p.room_hf = -100, p.decay_time = 1.49f, p.decay_hf_ratio = 0.83f, p.reflections = -2602;
        p.reflections_delay = 0.007f, p.reverb = 200, p.reverb_delay = 0.011f;
        r.set(p);
        CHECK(r.on());
        audio::Bins b;
        b.clear(48000);
        b.bin(audio::kI3dl2)[0] = 1.0f;
        r.process(b);
        float early = 0, tail = 0, late_tail = 0;
        for (uint32_t i = 0; i < 300; ++i) early += std::fabs(b.bin(audio::kFrontLeft)[i]);
        for (uint32_t i = 2400; i < 9600; ++i) tail += std::fabs(b.bin(audio::kBackRight)[i]);
        for (uint32_t i = 40000; i < 47200; ++i) late_tail += std::fabs(b.bin(audio::kBackRight)[i]);
        CHECK(early == 0.0f);  // nothing before 7 ms
        CHECK(tail > 0.01f && late_tail < tail * 0.1f);
        bool finite = true;
        for (uint32_t k = 0; k < audio::kMixBinCount; ++k)
            for (uint32_t i = 0; i < 48000; ++i) finite &= std::isfinite(b.bin(k)[i]);
        CHECK(finite);
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all audio tests passed\n");
    return failures != 0;
}
