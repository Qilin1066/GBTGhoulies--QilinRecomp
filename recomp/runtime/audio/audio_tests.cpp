// Audio engine tests: voices over a fake guest memory, no device needed.
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
    v.mix(mem.data(), mem.size(), out.data(), 8);
    CHECK(near(out[0], 0.5f) && near(out[1], 0.5f) && near(out[2], -0.5f) && near(out[4], 0.25f));
    CHECK(!v.playing() && near(out[8], 0.0f));

    // Volume in hundredths of a decibel: -600 is half amplitude (6.02 dB, near enough).
    out.assign(16, 0.0f);
    v.volume = -602;
    v.play(false);
    v.set_position(0);
    v.play(false);
    v.mix(mem.data(), mem.size(), out.data(), 1);
    CHECK(near(out[0], 0.25f, 1e-3f));
    v.volume = 0;

    // A loop keeps it playing past the end; a 24 kHz voice plays at half speed.
    out.assign(16, 0.0f);
    v.set_position(0);
    v.play(true);
    v.mix(mem.data(), mem.size(), out.data(), 8);
    CHECK(v.playing() && v.looping() && near(out[8], 0.5f));
    v.stop();
    f.rate = 24000;
    v.set_format(f);
    CHECK(v.pitch == -4096);
    v.set_data(0x100, sizeof pcm);
    out.assign(16, 0.0f);
    v.play(false);
    v.mix(mem.data(), mem.size(), out.data(), 3);
    CHECK(near(out[0], 0.5f) && near(out[2], 0.0f) && near(out[4], -0.5f));  // halfway between samples 0 and 1

    // Mix bins: a voice sent only to the front right is silent on the left.
    f.rate = 48000;
    v.set_format(f);
    v.set_data(0x100, sizeof pcm);
    v.mix_bins = {{audio::kFrontRight, 0}};
    out.assign(16, 0.0f);
    v.play(false);
    v.mix(mem.data(), mem.size(), out.data(), 1);
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
        sub.mix(mem.data(), mem.size(), o.data(), 4);
        CHECK(near(o[0], 0.25f, 1e-3f) && near(o[1], 0.25f, 1e-3f) && near(o[2], -0.25f, 1e-3f) && near(o[4], 0.125f, 1e-3f));
        sub.clear_input(4);  // nothing fed: silence
        std::vector<float> o2(8, 0.0f);
        sub.mix(mem.data(), mem.size(), o2.data(), 4);
        CHECK(near(o2[0], 0.0f) && sub.playing());
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all audio tests passed\n");
    return failures != 0;
}
