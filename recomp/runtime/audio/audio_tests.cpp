#include "../path.hpp"
#include <filesystem>
#include <fstream>
#include <limits>
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
    // A voice's front left and right bins for n frames, interleaved. Its
    // gains settled first (an empty block), so a new voice's fade-in and a
    // changed volume's ramp (tested on their own below) don't show.
    auto stereo = [&](audio::Voice& v, uint32_t n) {
        audio::Bins b;
        b.clear(n);
        v.mix(mem.data(), mem.size(), b, 0);
        v.mix(mem.data(), mem.size(), b, n);
        std::vector<float> o(std::max<size_t>(16, 2 * size_t(n)), 0.0f);
        for (uint32_t i = 0; i < n; ++i) o[2 * i] = b.bin(audio::kFrontLeft)[i], o[2 * i + 1] = b.bin(audio::kFrontRight)[i];
        return o;
    };

    // Empty submix blocks do not form references into empty sample storage.
    {
        audio::Voice source, target;
        source.set_format(audio::Format{});
        const int16_t sample = 123;
        std::memcpy(mem.data() + 0x100, &sample, sizeof sample);
        source.set_data(0x100, sizeof sample);
        source.play(true);
        target.make_submix(2);
        source.mix_into(mem.data(), mem.size(), target, 0);
    }
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
    // Headroom comes off the volume: 602 of it is the same half amplitude.
    v.headroom = 602;
    v.set_position(0);
    v.play(false);
    out = stereo(v, 1);
    CHECK(near(out[0], 0.25f, 1e-3f));
    v.headroom = 0;

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
    // Halfway between samples 0 and 1: the 8-point Lagrange weights at ½,
    // (−5, 49, −245, 1225, 1225, −245, 49, −5) / 2048, over 0, 0, 0, ½, −½, ¼, 0, 0.
    CHECK(near(out[0], 0.5f) && near(out[2], -245.0f / 2048 * 0.25f, 1e-6f) && near(out[4], -0.5f));

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

    // A 3D voice as DirectSound routes one (bins XtlkFL, XtlkBL, XtlkFR,
    // XtlkBR, centre, I3DL2, LFE): the left ear on routes 0-1, the right on
    // 2-3, each through its taps as written (0x40 is 1/16, 0x7F 31/32);
    // the I3DL2 filters' direct path on the others but 5, the room path on 5;
    // per-bin offsets (the back left 20 dB down here). An impulse through
    // a = 0x2000 (direct) gives 0.875, then × 0.125 a sample; a = 0xC000
    // (room) 0.25, then × 0.75 (the console's own figures).
    {
        const int16_t imp[4] = {16384, 0, 0, 0};
        std::memcpy(mem.data() + 0x200, imp, sizeof imp);
        audio::Voice d;
        d.set_format(audio::Format{});
        d.set_data(0x200, sizeof imp);
        d.mix_bins = {{audio::kXtlkFrontLeft, 0}, {audio::kXtlkBackLeft, 0}, {audio::kXtlkFrontRight, 0}, {audio::kXtlkBackRight, 0},
                      {audio::kFrontCenter, 0}, {audio::kI3dl2, 0}, {audio::kLowFrequency, 0}};
        d.three_d.active = true;
        d.three_d.hrtf = true;
        d.three_d.taps[0][0] = 0x40;  // left: 1/16, at once
        d.three_d.taps[1][2] = 0x7F;  // right: 31/32, two samples later
        d.three_d.fcb_set = true;
        d.three_d.fcb = 0xC000u << 16 | 0x2000u;
        d.three_d.bin_offset[audio::kXtlkBackLeft] = -2000;
        d.play(false);
        audio::Bins b;
        b.clear(4);
        d.mix(mem.data(), mem.size(), b, 0);  // settled: no fade-in
        d.mix(mem.data(), mem.size(), b, 4);
        const float direct0 = 0.5f * 0.875f, direct1 = direct0 * 0.125f;
        CHECK(near(b.bin(audio::kFrontCenter)[0], direct0, 1e-5f) && near(b.bin(audio::kFrontCenter)[1], direct1, 1e-5f));
        CHECK(near(b.bin(audio::kI3dl2)[0], 0.125f, 1e-5f) && near(b.bin(audio::kI3dl2)[1], 0.09375f, 1e-5f));
        CHECK(near(b.bin(audio::kLowFrequency)[0], direct0, 1e-5f));  // route 6: the direct path
        CHECK(near(b.bin(audio::kXtlkFrontLeft)[0], direct0 / 16, 1e-5f) && near(b.bin(audio::kXtlkFrontLeft)[1], direct1 / 16, 1e-5f));
        CHECK(near(b.bin(audio::kXtlkBackLeft)[0], direct0 / 16 * 0.1f, 2e-4f));
        CHECK(near(b.bin(audio::kXtlkFrontRight)[0], 0.0f) && near(b.bin(audio::kXtlkFrontRight)[2], direct0 * 31 / 32, 1e-5f));
        CHECK(near(b.bin(audio::kXtlkBackRight)[2], direct0 * 31 / 32, 1e-5f));
    }

    // The hardware's own numbers. Taps: sign and magnitude, 3-bit exponent,
    // 4-bit mantissa. Attenuation: 0 unity; past 48 dB a straight line to
    // silence at 63 dB (0xF00 12/16384, 0xF80 4, 0xFB0 1, 0xFC0 0).
    CHECK(audio::hrtf_tap(0x01) == 1.0f / 2048 && audio::hrtf_tap(0x20) == 1.0f / 64 && audio::hrtf_tap(0x40) == 1.0f / 16);
    CHECK(audio::hrtf_tap(0x7F) == 31.0f / 32 && audio::hrtf_tap(0x81) == -1.0f / 2048 && audio::hrtf_tap(0xC0) == -1.0f / 16);
    CHECK(audio::attenuation_gain(0) == 16384 && audio::attenuation_gain(0xF00) == 12 && audio::attenuation_gain(0xF80) == 4);
    CHECK(audio::attenuation_gain(0xFB0) == 1 && audio::attenuation_gain(0xFC0) == 0 && audio::attenuation_gain(0xFFF) == 0);
    CHECK(audio::attenuation_gain(6 * 64) == 2052 * 4);  // 6 dB: floor(4095 · 10^(−6/20)) = 2052

    // A new voice fades in from silence over its first 32 samples (the
    // console: 0, ¼, ½, ¾ and 31/32 of the way at samples 0, 8, 16, 24, 31);
    // a changed volume moves the same way, from the last block's gain.
    {
        std::vector<int16_t> dc(256, 16384);
        std::memcpy(mem.data() + 0x5000, dc.data(), dc.size() * 2);
        audio::Voice r;
        r.set_format(audio::Format{});
        r.set_data(0x5000, uint32_t(dc.size() * 2));
        r.play(true);
        audio::Bins b;
        b.clear(40);
        r.mix(mem.data(), mem.size(), b, 40);
        const float* l = b.bin(audio::kFrontLeft);
        CHECK(l[0] == 0.0f && near(l[8], 0.125f) && near(l[16], 0.25f) && near(l[24], 0.375f) && near(l[31], 0.5f * 31 / 32) && near(l[32], 0.5f));
        r.volume = -10000;
        b.clear(40);
        r.mix(mem.data(), mem.size(), b, 40);
        CHECK(near(l[0], 0.5f) && near(b.bin(audio::kFrontLeft)[16], 0.25f) && b.bin(audio::kFrontLeft)[32] == 0.0f);
    }

    // The parametric EQ (mode 2, mono: FCB): the state-variable filter as a
    // peaking EQ, y = x + G·q·band. DC passes as it is; a tone at the
    // centre frequency comes out (1 + G) times as loud (G = gain / 8192: 1 here).
    {
        std::vector<int16_t> tone(4800);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = int16_t(std::lround(8192.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * double(i) / 48000.0)));
        std::memcpy(mem.data() + 0x6000, tone.data(), tone.size() * 2);
        std::vector<int16_t> dc(1024, 8192);
        std::memcpy(mem.data() + 0x9000, dc.data(), dc.size() * 2);
        audio::Filter flt;
        flt.mode = 2;
        flt.q = 2;  // q = 1/4
        flt.coefficients[2] = uint32_t(uint16_t(int16_t(std::lround(4096.0 * std::log2(2.0 * std::sin(3.14159265358979 * 1000.0 / 48000.0))))));
        flt.coefficients[3] = 8192;
        audio::Voice t;
        t.set_format(audio::Format{});
        t.set_data(0x6000, uint32_t(tone.size() * 2));
        t.set_filter(flt);
        t.play(true);
        auto o = stereo(t, 4800);
        float peak = 0;
        for (uint32_t i = 2400; i < 4800; ++i) peak = std::max(peak, std::fabs(o[2 * i]));
        CHECK(near(peak, 0.5f, 0.03f));  // 0.25 doubled
        audio::Voice z;
        z.set_format(audio::Format{});
        z.set_data(0x9000, uint32_t(dc.size() * 2));
        z.set_filter(flt);
        z.play(true);
        o = stereo(z, 1024);
        CHECK(near(o[2 * 1023], 0.25f, 1e-3f));
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

    // Saturation at the mix bins' 24-bit full scale; the stereo fold's
    // limiter: below full scale, the signal sample for sample, 72 frames
    // (1.5 ms) late; above, nothing out of [−1, 1], and full gain again after.
    {
        audio::Bins b;
        b.clear(4);
        b.bin(0)[0] = 1.5f, b.bin(0)[1] = -2.0f, b.bin(0)[2] = 0.25f, b.bin(5)[3] = 1.0f;
        audio::saturate(b, 0, audio::kMixBinCount);
        CHECK(b.bin(0)[0] == 1.0f - 1.0f / 8388608.0f && b.bin(0)[1] == -1.0f && b.bin(0)[2] == 0.25f && b.bin(5)[3] < 1.0f);

        audio::Limiter lim;
        std::vector<float> x(2 * 48000);
        for (size_t i = 0; i < 48000; ++i) x[2 * i] = 0.9f * float(std::sin(0.01 * double(i))), x[2 * i + 1] = -x[2 * i];
        std::vector<float> y = x;
        lim.process(y.data(), 48000);
        bool same = true;
        for (size_t i = 72; i < 48000; ++i) same &= y[2 * i] == x[2 * (i - 72)] && y[2 * i + 1] == x[2 * (i - 72) + 1];
        CHECK(same);
        // A burst to 1.6 among quiet: held to full scale, and back to unity after it.
        std::vector<float> z(2 * 48000, 0.0f);
        for (size_t i = 0; i < 48000; ++i) z[2 * i] = z[2 * i + 1] = (i >= 2000 && i < 2400 ? 1.6f : 0.5f) * float(std::sin(0.05 * double(i)));
        std::vector<float> zin = z;
        audio::Limiter lim2;
        lim2.process(z.data(), 48000);
        float most = 0;
        for (float s : z) most = std::max(most, std::fabs(s));
        CHECK(most <= 1.0f && most > 0.99f);
        CHECK(z[2 * 47999] == zin[2 * (47999 - 72)]);  // recovered (the release: 50 ms, unity within 10^-5 after about 0.5 s)
    }

    // The filter at its unstable corner (the top of its range, damping
    // 2): bounded, never infinite; and a NaN in a bin comes out as silence.
    {
        audio::Svf svf;
        const double top = audio::Svf::frequency(0);
        bool finite = true;
        for (int i = 0; i < 48000; ++i) svf.step(i % 2 ? 0.5 : -0.5, top, 65535.0 / 32768.0), finite &= std::isfinite(svf.low) && std::fabs(svf.low) <= 16.0;
        CHECK(finite);
        audio::Bins b;
        b.clear(2);
        b.bin(0)[0] = std::nanf(""), b.bin(0)[1] = INFINITY;
        audio::saturate(b, 0, 1);
        CHECK(b.bin(0)[0] == 0.0f && b.bin(0)[1] < 1.0f);
    }

    // WAV captures: sizes stay valid while open; failures latch; conversion
    // handles non-finite samples without passing NaN to lround().
    {
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / fs::path(u8"qilin-wav-é-保存");
        fs::remove_all(dir); fs::create_directories(dir);
        auto u32 = [](const fs::path& path, size_t at) {
            std::ifstream file(path, std::ios::binary);
            file.seekg(std::streamoff(at));
            uint8_t bytes[4]{}; file.read(reinterpret_cast<char*>(bytes), 4);
            return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
        };
        const auto path = dir / "capture.wav";
        audio::WavWriter wav(utf8_path(path));
        CHECK(wav.ok() && fs::file_size(path) == 44 && u32(path, 4) == 36 && u32(path, 40) == 0);
        const float special[] = {1, -1, std::nanf(""), INFINITY, -INFINITY, 0.5f, 0, 0};
        CHECK(wav.write(special, 4));
        std::vector<float> silence(audio::kOutputRate, 0.0f);
        CHECK(wav.write(silence.data(), audio::kOutputRate / 2));
        CHECK(u32(path, 40) == (4 + audio::kOutputRate / 2) * 4);
        CHECK(wav.close() && wav.close() && !wav.ok());
        CHECK(fs::file_size(path) == 44 + (4 + audio::kOutputRate / 2) * 4);
        CHECK(u32(path, 44) == 0x80017fffu); // +1, -1
        CHECK(u32(path, 48) == 0x7fff0000u); // NaN -> silence, +infinity -> maximum
        CHECK(u32(path, 52) == 0x40008001u); // -infinity -> minimum, +0.5
        CHECK(!wav.write(special, 1));
        audio::WavWriter limit(utf8_path(dir / "limit.wav"));
        CHECK(!limit.write(nullptr, UINT32_MAX)); // reject before reading/allocating samples
        CHECK(!limit.close() && !limit.error().empty());
        CHECK(fs::file_size(dir / "limit.wav") == 44 && u32(dir / "limit.wav", 40) == 0);
        audio::WavWriter missing(utf8_path(dir / "missing" / "file.wav"));
        CHECK(!missing.ok() && !missing.close());
#ifndef _WIN32
        if (fs::exists("/dev/full")) {
            audio::WavWriter full("/dev/full");
            CHECK(!full.ok() && !full.write(special, 4) && !full.close());
        }
#endif
        fs::remove_all(dir);
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all audio tests passed\n");
    return failures != 0;
}
