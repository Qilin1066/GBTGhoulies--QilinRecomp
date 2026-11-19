// DirectSound (the XDK's DSOUND library), replaced natively over the audio
// engine (recomp/runtime/audio). docs/audio.md describes the design.
//
// XACT, lifted, calls these; so does the game, once. DirectSound on the Xbox
// programs the APU's voice processor and runs effects on its DSP; here the
// voices are the audio engine's, mixed in software, with the DSP's effects
// (the I3DL2 reverb) native. The audio clock is the host's: the mixer runs
// on its own thread, a few milliseconds behind it, and on every DirectSound
// call it catches up to the time since the device was created, so a sound
// plays for as long as it does on the console and GetStatus reports its end
// when it comes. One lock covers the mixer's state; guest code (a stream's
// callback) never runs under it.
//
// Settings that change a voice's sound but aren't applied (LFOs, the
// parametric EQ) are recorded and named once at run time. Anything not
// modelled at all stops the runtime with its name.
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio.hpp"
#include "audio_out.hpp"
#include "host.hpp"
#include "sched.hpp"
#include "strf.hpp"

namespace qilin::host {
namespace {

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }
uint32_t ld(const rt::Context& c, uint32_t a) { return rt::ld32(mem(c), a, 0); }
uint16_t ld16(const rt::Context& c, uint32_t a) { return rt::ld16(mem(c), a, 0); }
void st(const rt::Context& c, uint32_t a, uint32_t v) { rt::st32(mem(c), a, v, 0); }

[[noreturn]] void unmodelled(const char* fn, const std::string& what) {
    static std::string why;
    why = strf("%s: %s isn't modelled yet", fn, what.c_str());
    rt::trap(0, why.c_str());
}

void notice_once(const std::string& what) {
    static std::set<std::string> said;
    if (said.insert(what).second) std::printf("qilin_run: audio: %s isn't applied yet\n", what.c_str());
}

constexpr uint32_t kDsOk = 0;
// XMEDIAPACKET status values (the XDK's: S_OK, E_PENDING, E_ABORT).
constexpr uint32_t kPacketSuccess = 0, kPacketPending = 0x8000000Au, kPacketFlushed = 0x80004004u;

// ---------------------------------------------------------------- objects
struct Buffer {
    uint32_t refs = 1;
    uint32_t flags = 0;
    uint32_t output = 0;  // the submix buffer it feeds (SetOutputBuffer), or 0 for the mix bins
    audio::Voice voice;
    // A stream (CreateSoundStream): its callback and context, how many packets
    // it takes at once, and the packets submitted and not yet completed, by id.
    bool stream = false;
    bool resume_on_start = false;  // Pause(3) on a voice not yet running: resume when it starts
    uint32_t callback = 0, context = 0, max_packets = 0;
    struct Packet {
        uint32_t completed_size = 0, status = 0, event = 0, context = 0;  // XMEDIAPACKET's pointers and context
    };
    std::map<uint32_t, Packet> packets;
    bool flush_after_release = false;  // FlushEx with the envelope: flush once the release has played
    // DirectSound's 3D voice data (Set3DVoiceData): the DS3DVOICEDATA
    // dwords after its flags, as the original keeps them, and whether the
    // voice uses them (Use3DVoiceData).
    uint32_t d3[14] = {};
    bool has_3d = false, use_3d = false;
};

// A packet the mixer finished (or a flush dropped), waiting to be reported
// where the console would report it: in a DPC, so not while the processor's
// level is raised to DISPATCH_LEVEL or above.
struct Completion {
    uint32_t stream, id, bytes, status;
};

struct DirectSound {
    std::mutex m;  // everything below; never held while guest code runs
    uint32_t object = 0, refs = 0;
    std::map<uint32_t, std::unique_ptr<Buffer>> buffers;  // by guest address
    std::chrono::steady_clock::time_point epoch;
    platform::AudioOut* out = nullptr;  // the speakers, if there's a sound device
    uint64_t mixed = 0;  // output frames mixed so far
    uint32_t next_packet = 1;
    std::vector<Completion> completions;
    std::unique_ptr<audio::WavWriter> wav;
    std::string dump_path;
    uint8_t* mem = nullptr;  // guest RAM, for the mixer thread
    size_t mem_size = 0;
    audio::Bins bins;
    audio::I3dl2Reverb reverb;
    std::vector<float> speakers, stereo;
    std::thread mixer;
    std::atomic<bool> quit{false};
    std::condition_variable wake;
    ~DirectSound() {
        if (mixer.joinable()) {
            quit = true;
            wake.notify_all();
            mixer.join();
        }
    }
};
DirectSound& ds() {
    static DirectSound d;
    return d;
}
using Lock = std::unique_lock<std::mutex>;

// Guest memory for object headers, so the game and XACT hold real pointers.
uint32_t guest_object() {
    const uint32_t a = contiguous(0x1000, 0, kPhysicalLimit - 1, 0x1000, 4);
    if (!a) rt::trap(0, "DirectSound: out of guest memory for objects");
    return a;
}

Buffer& buffer(rt::Context& c, uint32_t a, const char* fn) {
    auto it = ds().buffers.find(a);
    if (it == ds().buffers.end()) unmodelled(fn, strf("a buffer the runtime didn't create (%08X)", a));
    (void)c;
    return *it->second;
}

// The audio clock: 48 kHz frames since DirectSoundCreate, on the host's clock.
uint64_t audio_now() {
    const DirectSound& d = ds();
    if (!d.object) return 0;
    return uint64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - d.epoch).count() * audio::kOutputRate);
}

// The APU's global sample counter (NV_PAPU_XGSCNT): 48 kHz frames since the
// APU started, which XACT reads directly as its clock. Here it's the mixer's
// clock, so XACT's timing and the sound it hears stay in step.
uint32_t apu_global_sample_count() { return uint32_t(audio_now()); }

// One block of the mix: every voice into the bins (submixes' sources
// first, into their inputs too), the reverb on its bin, the crosstalk canceller's
// inputs to their speakers (the binaural signal as it is: the canceller,
// for speakers, isn't applied), then the speakers to the device: 5.1 as
// they are (front left, right, centre, LFE, back left, right: SDL's
// order too), or folded to stereo (the centre at -3 dB into both, the
// back bins into their sides, the LFE left out).
void mix_block(DirectSound& d, uint32_t n) {
    d.bins.clear(n);
    for (auto& [addr, b] : d.buffers)
        if (b->voice.submix()) b->voice.clear_input(n);
    for (auto& [addr, b] : d.buffers)
        if (b->output) b->voice.mix(d.mem, d.mem_size, d.bins, n, &d.buffers.at(b->output)->voice);
    for (auto& [addr, b] : d.buffers)
        if (!b->output) b->voice.mix(d.mem, d.mem_size, d.bins, n);
    d.reverb.process(d.bins);
    static constexpr uint32_t kXtlkTo[4] = {audio::kFrontLeft, audio::kFrontRight, audio::kBackLeft, audio::kBackRight};
    for (uint32_t k = 0; k < 4; ++k) {
        const float* from = d.bins.bin(audio::kXtlkFrontLeft + k);
        float* to = d.bins.bin(kXtlkTo[k]);
        for (uint32_t i = 0; i < n; ++i) to[i] += from[i];
    }
    const float* fl = d.bins.bin(audio::kFrontLeft);
    const float* fr = d.bins.bin(audio::kFrontRight);
    const float* fc = d.bins.bin(audio::kFrontCenter);
    const float* lfe = d.bins.bin(audio::kLowFrequency);
    const float* bl = d.bins.bin(audio::kBackLeft);
    const float* br = d.bins.bin(audio::kBackRight);
    d.stereo.resize(size_t(n) * 2);
    for (uint32_t i = 0; i < n; ++i) {
        d.stereo[2 * i] = fl[i] + fc[i] * 0.70710678f + bl[i];
        d.stereo[2 * i + 1] = fr[i] + fc[i] * 0.70710678f + br[i];
    }
    if (d.wav) d.wav->write(d.stereo.data(), n);
    if (d.out && d.out->channels() == 6) {
        d.speakers.resize(size_t(n) * 6);
        for (uint32_t i = 0; i < n; ++i) {
            float* o = &d.speakers[size_t(i) * 6];
            o[0] = fl[i], o[1] = fr[i], o[2] = fc[i], o[3] = lfe[i], o[4] = bl[i], o[5] = br[i];
        }
        d.out->push(d.speakers.data(), n);
    } else if (d.out) {
        d.out->push(d.stereo.data(), n);
    }
}

// Brings the mix up to now (the lock held).
void catch_up_locked() {
    DirectSound& d = ds();
    if (!d.object) return;
    const uint64_t target = audio_now();
    while (d.mixed < target) {
        const uint32_t n = uint32_t(std::min<uint64_t>(target - d.mixed, 256));
        mix_block(d, n);
        for (auto& [addr, b] : d.buffers) {
            if (!b->stream) continue;
            for (const audio::Voice::Done& done : b->voice.take_completed()) d.completions.push_back({addr, done.id, done.bytes, kPacketSuccess});
            if (b->flush_after_release && !b->voice.playing()) {
                b->flush_after_release = false;
                for (const audio::Voice::Done& done : b->voice.flush()) d.completions.push_back({addr, done.id, done.bytes, kPacketFlushed});
            }
        }
        d.mixed += n;
    }
}
void catch_up(rt::Context&) { catch_up_locked(); }

// The mixer's own thread: it keeps the mix (and the device's queue) up to
// the clock between the game's DirectSound calls.
void mixer_thread() {
    DirectSound& d = ds();
    Lock l(d.m);
    while (!d.quit) {
        catch_up_locked();
        d.wake.wait_for(l, std::chrono::milliseconds(4));
    }
}

// Reports finished packets, as the console's DPC does: the completed size
// and status into the packet's words, its event signalled, the stream's
// callback called (pStreamContext, pPacketContext, dwStatus). Only below
// DISPATCH_LEVEL, as a DPC would wait for the level to drop.
void deliver(rt::Context& c) {
    struct Call {
        uint32_t event, callback, context, packet_context, status;
    };
    std::vector<Call> calls;
    {
        Lock l(ds().m);
        DirectSound& d = ds();
        if (d.completions.empty()) return;
        if (rt::ld8(mem(c), c.fs_base + 0x24, 0) >= 2) return;  // the KPCR's Irql
        std::vector<Completion> now;
        now.swap(d.completions);
        for (const Completion& k : now) {
            auto it = d.buffers.find(k.stream);
            if (it == d.buffers.end()) continue;
            Buffer& b = *it->second;
            auto p = b.packets.find(k.id);
            if (p == b.packets.end()) continue;
            const Buffer::Packet pk = p->second;
            b.packets.erase(p);
            if (pk.completed_size) st(c, pk.completed_size, k.bytes);
            if (pk.status) st(c, pk.status, k.status);
            calls.push_back({pk.event, b.callback, b.context, pk.context, k.status});
        }
    }
    // Outside the lock: the event can switch threads, the callback is guest code.
    for (const Call& k : calls) {
        if (k.event) sched::set_event(k.event);  // as KeSetEvent: what waits on it wakes
        if (k.callback) call_guest(c, k.callback, {k.context, k.packet_context, k.status});
    }
}

// ---------------------------------------------------------------- formats
// WAVEFORMATEX: wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec,
// nBlockAlign, wBitsPerSample, cbSize.
audio::Format read_format(rt::Context& c, uint32_t wfx, const char* fn) {
    audio::Format f;
    const uint32_t tag = ld16(c, wfx), channels = ld16(c, wfx + 2), bits = ld16(c, wfx + 14);
    f.rate = ld(c, wfx + 4);
    f.channels = channels;
    if (channels < 1 || channels > 2) unmodelled(fn, strf("%u channels", channels));
    if (tag == 1 && (bits == 8 || bits == 16)) {
        f.codec = audio::Format::Codec::Pcm;
        f.bits = bits;
    } else if (tag == 0x69) {
        f.codec = audio::Format::Codec::XboxAdpcm;
        f.bits = 4;
    } else {
        unmodelled(fn, strf("wave format tag %#x with %u bits", tag, bits));
    }
    return f;
}

// DSMIXBINS: dwMixBinCount, lpMixBinVolumePairs ({dwMixBin, lVolume} each).
std::vector<audio::MixBinVolume> read_mix_bins(rt::Context& c, uint32_t p) {
    std::vector<audio::MixBinVolume> v;
    if (!p) return v;
    const uint32_t n = ld(c, p), pairs = ld(c, p + 4);
    for (uint32_t i = 0; i < n && i < 32; ++i) {
        const uint32_t bin = ld(c, pairs + 8 * i);
        v.push_back({bin, int32_t(ld(c, pairs + 8 * i + 4))});
        if (bin >= audio::kFxSend0) notice_once(strf("effect send %u (the effects image has only the I3DL2 reverb and the crosstalk canceller)", bin));
        if (bin >= audio::kMixBinCount) unmodelled("DirectSound", strf("mix bin %u", bin));
    }
    return v;
}

// ---------------------------------------------------------------- DirectSound
// HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND*, LPUNKNOWN): the one
// device; a second create returns it with another reference, as on the Xbox.
void direct_sound_create(rt::Context& c) {
    Lock lock_(ds().m);
    DirectSound& d = ds();
    if (!d.object) {
        d.object = guest_object();
        d.epoch = std::chrono::steady_clock::now();
        d.mem = c.mem, d.mem_size = c.mem_size;
        d.mixer = std::thread(mixer_thread);
        if (!d.dump_path.empty()) {
            d.wav = std::make_unique<audio::WavWriter>(d.dump_path);
            if (!d.wav->ok()) std::fprintf(stderr, "qilin: can't write %s\n", d.dump_path.c_str());
        }
        std::printf("qilin_run: DirectSound: %u Hz software mix on its own thread, to %s%s%s\n", audio::kOutputRate,
                    d.out && d.out->channels() == 6 ? "5.1" : "stereo", d.wav ? ", written to " : "", d.wav ? d.dump_path.c_str() : "");
    }
    ++d.refs;
    st(c, arg(c, 1), d.object);
    ret(c, kDsOk, 12);
}

void ds_release(rt::Context& c) {
    Lock lock_(ds().m);
    DirectSound& d = ds();
    if (arg(c, 0) != d.object || d.refs == 0) unmodelled("IDirectSound_Release", "releasing an object that isn't the device");
    --d.refs;  // the device stays: freeing it isn't modelled, and a title creates it once
    ret(c, d.refs, 4);
}

// HRESULT IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND, DWORD dwMixBin, DWORD dwHeadroom):
// the bin's headroom (its attenuation before the speakers), recorded.
void set_mix_bin_headroom(rt::Context& c) {
    Lock lock_(ds().m);
    if (arg(c, 2)) notice_once(strf("mix bin headroom (%u on bin %u)", arg(c, 2), arg(c, 1)));
    ret(c, kDsOk, 12);
}

// HRESULT IDirectSound_CreateSoundBuffer(LPDIRECTSOUND, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN)
// DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin.
void create_sound_buffer(rt::Context& c) {
    Lock lock_(ds().m);
    const uint32_t desc = arg(c, 1), out = arg(c, 2);
    const uint32_t flags = ld(c, desc + 4), bytes = ld(c, desc + 8), wfx = ld(c, desc + 12), bins = ld(c, desc + 16);
    if (flags & 0x80000) unmodelled("IDirectSound_CreateSoundBuffer", "an effects input buffer (DSBCAPS_FXIN)");
    if (bytes) unmodelled("IDirectSound_CreateSoundBuffer", "a buffer with its own memory (dwBufferBytes != 0)");
    auto b = std::make_unique<Buffer>();
    b->flags = flags;
    if (flags & 0x2000) {  // DSBCAPS_MIXIN: a submix destination, with the format's channels
        b->voice.make_submix(wfx ? read_format(c, wfx, "IDirectSound_CreateSoundBuffer").channels : 1);
    } else if (wfx) {
        b->voice.set_format(read_format(c, wfx, "IDirectSound_CreateSoundBuffer"));
    }
    b->voice.mix_bins = read_mix_bins(c, bins);
    const uint32_t obj = guest_object();
    ds().buffers[obj] = std::move(b);
    st(c, out, obj);
    ret(c, kDsOk, 16);
}

void buffer_release(rt::Context& c) {
    Lock lock_(ds().m);
    const uint32_t a = arg(c, 0);
    Buffer& b = buffer(c, a, "IDirectSoundBuffer_Release");
    const uint32_t refs = --b.refs;
    if (refs == 0) {
        catch_up(c);
        for (auto& [addr, other] : ds().buffers)
            if (other->output == a) unmodelled("IDirectSoundBuffer_Release", "releasing a submix buffer other buffers still feed");
        ds().buffers.erase(a);  // its guest header page stays allocated: freeing contiguous memory isn't modelled
    }
    ret(c, refs, 4);
}

// HRESULT IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER, LPVOID pvBufferData, DWORD dwBufferBytes)
void set_buffer_data(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_SetBufferData");
    if (b.voice.submix()) unmodelled("IDirectSoundBuffer_SetBufferData", "sample data for a submix buffer");
    b.voice.stop();
    b.voice.set_data(arg(c, 1), arg(c, 2));
    ret(c, kDsOk, 12);
}

// HRESULT IDirectSoundBuffer_SetFormat(LPDIRECTSOUNDBUFFER, LPCWAVEFORMATEX)
void set_format(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_SetFormat");
    if (b.voice.submix()) unmodelled("IDirectSoundBuffer_SetFormat", "a submix buffer's format");
    b.voice.set_format(read_format(c, arg(c, 1), "IDirectSoundBuffer_SetFormat"));
    ret(c, kDsOk, 8);
}

// A 3D voice's volumes and pitch, as the original computes them from its
// 3D voice data (CMcpxVoiceClient's volume calculation, 0x1CBDC1): with
// d[k] the dword at 4·k, a base of d1 + d2; the crosstalk front bins
// d7 + d5 + d3 more, the crosstalk back bins d7 + d4, the centre d7 + d6 +
// d3, the I3DL2 send d8; each sum held to -10000..0 and added to the bin's
// volume. The pitch takes d9 (Doppler).
void apply_3d(Buffer& b) {
    audio::Voice3d& v = b.voice.three_d;
    v.active = b.use_3d && b.has_3d;
    const auto& d = b.d3;
    auto clamp = [](int64_t x) { return int32_t(std::clamp<int64_t>(x, audio::kVolumeMin, 0)); };
    auto s = [&](uint32_t k) { return int64_t(int32_t(d[k])); };
    const int32_t base = clamp(s(1) + s(2));
    for (uint32_t bin = 0; bin < audio::kMixBinCount; ++bin) v.bin_offset[bin] = base;
    v.bin_offset[audio::kXtlkFrontLeft] = v.bin_offset[audio::kXtlkFrontRight] = clamp(s(7) + s(5) + s(3) + base);
    v.bin_offset[audio::kXtlkBackLeft] = v.bin_offset[audio::kXtlkBackRight] = clamp(s(7) + s(4) + base);
    v.bin_offset[audio::kFrontCenter] = clamp(s(7) + s(6) + s(3) + base);
    v.bin_offset[audio::kI3dl2] = clamp(s(8) + base);
    v.pitch_offset = int32_t(d[9]);
}

#define BUFFER_SETTER(name, bytes, ...)                 \
    void name(rt::Context& c) {                         \
        Lock lock_(ds().m);                             \
        catch_up(c);                                    \
        Buffer& b = buffer(c, arg(c, 0), #name);        \
        (void)b;                                        \
        __VA_ARGS__;                                    \
        ret(c, kDsOk, bytes);                           \
    }

BUFFER_SETTER(set_volume, 8, b.voice.volume = std::clamp(int32_t(arg(c, 1)), audio::kVolumeMin, 0))
BUFFER_SETTER(set_pitch, 8, b.voice.pitch = int32_t(arg(c, 1)))
BUFFER_SETTER(set_headroom, 8, b.voice.headroom = int32_t(arg(c, 1)))
BUFFER_SETTER(set_mix_bins, 8, b.voice.mix_bins = read_mix_bins(c, arg(c, 1)))
// SetMixBinVolumes changes the volumes of bins the voice already feeds.
BUFFER_SETTER(set_mix_bin_volumes, 8, {
    for (const audio::MixBinVolume& v : read_mix_bins(c, arg(c, 1)))
        for (audio::MixBinVolume& m : b.voice.mix_bins)
            if (m.bin == v.bin) m.volume = v.volume;
})
BUFFER_SETTER(set_lfo, 8, notice_once("LFOs (pitch and amplitude modulation)"))
// DSENVELOPEDESC: dwEG (1 amplitude, 2 the multi envelope: pitch and
// filter), dwMode, dwDelay, dwAttack, dwHold, dwDecay, dwRelease,
// dwSustain, lPitchScale, lFilterCutOff.
BUFFER_SETTER(set_eg, 8, {
    const uint32_t p = arg(c, 1);
    if (ld(c, p) != 1) {
        notice_once("the multi envelope (pitch and filter modulation)");
    } else {
        audio::Envelope e;
        e.mode = ld(c, p + 4), e.delay = ld(c, p + 8) & 0xFFF, e.attack = ld(c, p + 12) & 0xFFF, e.hold = ld(c, p + 16) & 0xFFF;
        e.decay = ld(c, p + 20) & 0xFFF, e.release = ld(c, p + 24) & 0xFFF, e.sustain = ld(c, p + 28) & 0xFF;
        if (e.mode > 3) unmodelled("SetEG", strf("envelope mode %u", e.mode));
        b.voice.set_envelope(e);
    }
})
// DSFILTERDESC: dwMode, dwQCoefficient, adwCoefficients[4].
BUFFER_SETTER(set_filter, 8, {
    const uint32_t p = arg(c, 1);
    audio::Filter f;
    f.mode = ld(c, p), f.q = ld(c, p + 4);
    for (uint32_t k = 0; k < 4; ++k) f.coefficients[k] = ld(c, p + 8 + 4 * k);
    if (f.mode > 3) unmodelled("SetFilter", strf("filter mode %u", f.mode));
    if (f.mode >= 2) notice_once("the voice filter's parametric EQ");
    b.voice.set_filter(f);
})
BUFFER_SETTER(use_3d_voice_data, 8, {
    b.use_3d = arg(c, 1) != 0;
    apply_3d(b);
})
BUFFER_SETTER(set_loop_region, 12, b.voice.set_loop_region(arg(c, 1), arg(c, 2)))
BUFFER_SETTER(set_play_region, 12, b.voice.set_play_region(arg(c, 1), arg(c, 2)))
BUFFER_SETTER(set_current_position, 8, b.voice.set_position(arg(c, 1)))
BUFFER_SETTER(stop, 4, b.voice.stop())

// HRESULT Set3DVoiceData(LPDIRECTSOUNDBUFFER, LPCDS3DVOICEDATA): the 3D
// calculator's results (the game runs DirectSound's own, lifted), as the
// original (CDirectSoundVoice_Set3DVoiceData, 0x1C5E64) takes them: the
// flags, then each field the flags name (bit 0 d1; 1 d2; 2 d3, d4; 3 d5,
// d6; 4 d7, d8; 5 d9; 6 the head-related filter's two angles; 7 the 3D
// filter's coefficients). The head-related filter's taps and interaural
// delay come from the HRTF table the game selected (DirectSoundUseFullHRTF),
// through the original's own lookup (the function at 0x1E3B88, lifted),
// called before the mixer's lock is taken.
constexpr uint32_t kHrtfLookup = 0x1E3B88, kHrtfMode = 0x1E34AC, kHrtfAzimuthSplit = 0x32B100;
void set_3d_voice_data(rt::Context& c) {
    const char* fn = "Set3DVoiceData";
    const uint32_t a = arg(c, 0), p = arg(c, 1);
    const uint32_t flags = ld(c, p);
    // The head-related filter, when the angles change.
    bool hrtf = false;
    int8_t taps[2][31] = {};
    int32_t itd = 0;
    if (flags & 0x40) {
        const uint32_t lookup = ld(c, kHrtfLookup);
        if (!lookup) unmodelled(fn, "a 3D voice's angles before the game has chosen an HRTF table");
        const uint32_t mode = ld(c, kHrtfMode);
        const uint32_t full = ((mode & 0xFFFF) == 2 || (mode & 0x10000)) ? 1 : 0;
        const uint32_t azimuth = ld(c, p + 0x28), elevation = ld(c, p + 0x2C);
        const uint32_t esp = c.esp;
        c.esp -= 16;  // the two coefficient pointers it returns, on the guest's stack
        const uint32_t out = c.esp;
        const uint32_t saved[4] = {c.eax, c.ecx, c.edx, c.ebx};
        call_guest(c, lookup, {azimuth, elevation, full, out});
        c.eax = saved[0], c.ecx = saved[1], c.edx = saved[2], c.ebx = saved[3];
        const uint32_t left = ld(c, out), right = ld(c, out + 4);
        c.esp = esp;
        for (uint32_t k = 0; k < 31; ++k) {
            taps[0][k] = int8_t(rt::ld8(mem(c), left + k, 0));
            taps[1][k] = int8_t(rt::ld8(mem(c), right + k, 0));
        }
        // The delay: the first table's when the azimuth is past the split, else the second's, negated.
        const float split = std::bit_cast<float>(ld(c, kHrtfAzimuthSplit));
        itd = std::bit_cast<float>(azimuth) >= split ? int32_t(rt::ld8(mem(c), left + 31, 0)) : -int32_t(rt::ld8(mem(c), right + 31, 0));
        hrtf = true;
    }
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, a, fn);
    if (flags & 0xFF) {
        static constexpr uint8_t kFirst[8] = {1, 2, 3, 5, 7, 9, 10, 12}, kCount[8] = {1, 1, 2, 2, 2, 1, 2, 2};
        for (uint32_t bit = 0; bit < 8; ++bit)
            if (flags & (1u << bit))
                for (uint32_t k = 0; k < kCount[bit]; ++k) b.d3[kFirst[bit] + k] = ld(c, p + 4 * (kFirst[bit] + k));
        b.d3[0] = flags;
        b.has_3d = true;
    }
    if (flags & 0x80) notice_once("the 3D voice data's filter (DS3DVOICEDATA bit 7)");
    if (hrtf) {
        audio::Voice3d& v = b.voice.three_d;
        std::memcpy(v.taps, taps, sizeof taps);
        v.itd = itd;
        v.hrtf = true;
    }
    apply_3d(b);
    ret(c, kDsOk, 8);
}

// HRESULT IDirectSoundBuffer_SetFrequency(LPDIRECTSOUNDBUFFER, DWORD dwFrequency):
// the pitch for that rate (XAudioCalculatePitch), 0 the format's own.
BUFFER_SETTER(set_frequency, 8, {
    const uint32_t f = arg(c, 1);
    b.voice.pitch = audio::pitch_for_rate(f ? f : b.voice.format().rate);
})

// HRESULT IDirectSoundBuffer_GetCurrentPosition(LPDIRECTSOUNDBUFFER, LPDWORD pdwPlayCursor, LPDWORD pdwWriteCursor):
// the play cursor where the voice is; the write cursor with it (the APU
// reads the data as it plays, so writing is safe anywhere after it).
void get_current_position(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_GetCurrentPosition");
    const uint32_t pos = b.voice.position_bytes();
    if (arg(c, 1)) st(c, arg(c, 1), pos);
    if (arg(c, 2)) st(c, arg(c, 2), pos);
    ret(c, kDsOk, 12);
}

// HRESULT IDirectSoundBuffer_Lock(LPDIRECTSOUNDBUFFER, DWORD dwOffset, DWORD dwBytes,
//     LPVOID* ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID* ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags):
// the buffer's data is the game's own memory (SetBufferData), so a lock is
// pointers into it, wrapping at its end: DSBLOCK_FROMWRITECURSOR (1) from
// the play position, DSBLOCK_ENTIREBUFFER (2) all of it.
void lock_buffer(rt::Context& c) {
    const char* fn = "IDirectSoundBuffer_Lock";
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), fn);
    const uint32_t size = b.voice.data_bytes(), flags = arg(c, 7);
    if (!size) unmodelled(fn, "locking a buffer without data");
    uint32_t offset = arg(c, 1), bytes = arg(c, 2);
    if (flags & 1) offset = b.voice.position_bytes();
    if (flags & 2) offset = 0, bytes = size;
    offset %= size;
    bytes = std::min(bytes, size);
    const uint32_t first = std::min(bytes, size - offset);
    st(c, arg(c, 3), b.voice.data_address() + offset);
    st(c, arg(c, 4), first);
    if (arg(c, 5)) st(c, arg(c, 5), bytes > first ? b.voice.data_address() : 0);
    if (arg(c, 6)) st(c, arg(c, 6), bytes - first);
    ret(c, kDsOk, 32);
}

// HRESULT IDirectSoundBuffer_Unlock(LPDIRECTSOUNDBUFFER, LPVOID, DWORD, LPVOID, DWORD):
// nothing to do: the mixer reads the memory as it plays.
void unlock_buffer(rt::Context& c) { ret(c, kDsOk, 20); }

// HRESULT IDirectSoundBuffer_SetOutputBuffer(LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER pOutputBuffer):
// the voice feeds a submix buffer (DSBCAPS_MIXIN) instead of the mix bins;
// null sends it back to the mix bins (the default).
void set_output_buffer(rt::Context& c) {
    Lock lock_(ds().m);
    const char* fn = "IDirectSoundBuffer_SetOutputBuffer";
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), fn);
    const uint32_t to = arg(c, 1);
    if (to) {
        const Buffer& target = buffer(c, to, fn);
        if (!target.voice.submix()) unmodelled(fn, "an output buffer that isn't a submix (DSBCAPS_MIXIN) buffer");
        if (b.voice.submix()) unmodelled(fn, "a submix feeding another submix");
    }
    b.output = to;
    ret(c, kDsOk, 8);
}

// HRESULT IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER, DWORD, DWORD, DWORD dwFlags):
// DSBPLAY_LOOPING (1), DSBPLAY_FROMSTART (2), DSBPLAY_SYNCHPLAYBACK (4).
void play(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_Play");
    const uint32_t flags = arg(c, 3);
    if (flags & 4) unmodelled("IDirectSoundBuffer_Play", "DSBPLAY_SYNCHPLAYBACK");
    if (flags & 2) b.voice.set_position(0);
    b.voice.play((flags & 1) != 0);
    ret(c, kDsOk, 16);
}

// HRESULT IDirectSoundBuffer_StopEx(LPDIRECTSOUNDBUFFER, REFERENCE_TIME rtTimeStamp, DWORD dwFlags):
// DSBSTOPEX_IMMEDIATE (0), DSBSTOPEX_ENVELOPE (1: the release phase; without
// an envelope, at once), DSBSTOPEX_RELEASEWAVEFORM (2: play on past the loop to the end).
void stop_ex(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_StopEx");
    if (arg(c, 1) || arg(c, 2)) unmodelled("IDirectSoundBuffer_StopEx", "stopping at a time stamp");
    const uint32_t flags = arg(c, 3);
    if (flags & 2) {
        if (b.voice.playing()) b.voice.play(false);  // leave the loop, finish the region
    } else if (flags & 1) {
        b.voice.release();  // the envelope's release, then it stops
    } else {
        b.voice.stop();
    }
    ret(c, kDsOk, 16);
}

// HRESULT IDirectSoundBuffer_GetStatus(LPDIRECTSOUNDBUFFER, LPDWORD pdwStatus):
// DSBSTATUS_PLAYING (1), DSBSTATUS_LOOPING (4).
void get_status(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_GetStatus");
    st(c, arg(c, 1), b.voice.playing() ? (b.voice.looping() ? 5u : 1u) : 0u);
    ret(c, kDsOk, 8);
}

// HRESULT IDirectSound_CommitDeferredSettings(LPDIRECTSOUND): settings take effect at once here.
void commit_deferred_settings(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    ret(c, kDsOk, 4);
}

// VOID DirectSoundDoWork(): DirectSound's periodic work; here, mixing up to now.
void do_work(rt::Context& c) {
    {
        Lock lock_(ds().m);
        catch_up(c);
    }
    deliver(c);
    ret(c, c.eax, 0);
}

// HRESULT IDirectSound_DownloadEffectsImage(LPDIRECTSOUND, LPCVOID pvImageBuffer,
//     DWORD dwImageSize, LPCDSEFFECTIMAGELOC pImageLoc, LPDSEFFECTIMAGEDESC* ppImageDesc):
// loads a program for the APU's DSP. The game's image holds the standard
// effects; DSEFFECTIMAGELOC says where the I3DL2 reverb and the crosstalk
// canceller are in it. The DSP isn't emulated: the reverb is native, driven
// by the same settings (SetI3DL2Listener, the I3DL2 mix bin); the crosstalk
// canceller's inputs go to their speakers as they are (mix_block).
void download_effects_image(rt::Context& c) {
    Lock lock_(ds().m);
    const uint32_t size = arg(c, 2), loc = arg(c, 3), desc_out = arg(c, 4);
    if (desc_out) unmodelled("IDirectSound_DownloadEffectsImage", "returning the image's effect descriptions (ppImageDesc)");
    std::printf("qilin_run: DirectSound: effects image of %u bytes (I3DL2 reverb at effect %d, crosstalk at %d), run natively\n", size,
                loc ? int32_t(ld(c, loc)) : -1, loc ? int32_t(ld(c, loc + 4)) : -1);
    ret(c, kDsOk, 20);
}

// HRESULT IDirectSound_SetI3DL2Listener(LPDIRECTSOUND, LPCDSI3DL2LISTENER, DWORD dwApply):
// the environmental reverb's parameters, recorded.
void set_i3dl2_listener(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    // DSI3DL2LISTENER: lRoom, lRoomHF, flRoomRolloffFactor, flDecayTime,
    // flDecayHFRatio, lReflections, flReflectionsDelay, lReverb,
    // flReverbDelay, flDiffusion, flDensity, flHFReference.
    if (const uint32_t p = arg(c, 1)) {
        auto i = [&](uint32_t k) { return int32_t(ld(c, p + 4 * k)); };
        auto f = [&](uint32_t k) { return std::bit_cast<float>(ld(c, p + 4 * k)); };
        audio::I3dl2Params r;
        r.room = i(0), r.room_hf = i(1), r.room_rolloff = f(2), r.decay_time = f(3), r.decay_hf_ratio = f(4);
        r.reflections = i(5), r.reflections_delay = f(6), r.reverb = i(7), r.reverb_delay = f(8);
        r.diffusion = f(9), r.density = f(10), r.hf_reference = f(11);
        ds().reverb.set(r);
    }
    ret(c, kDsOk, 12);
}

// ---------------------------------------------------------------- streams
// The stream object is the original's shape where callers look: its first
// word is CDirectSoundStream's vtable (0x20A038: AddRef, Release, GetInfo,
// GetStatus, Process, Discontinuity, Flush), the second its inner
// interface's (0x20A028), so XACT's calls through the vtable reach the
// replacements registered at those methods' addresses.
constexpr uint32_t kStreamVtable = 0x20A038, kStreamInnerVtable = 0x20A028;

// HRESULT IDirectSound_CreateSoundStream(LPDIRECTSOUND, LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM*, LPUNKNOWN)
// DSSTREAMDESC: dwFlags, dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins.
void create_sound_stream(rt::Context& c) {
    Lock lock_(ds().m);
    const char* fn = "IDirectSound_CreateSoundStream";
    catch_up(c);
    const uint32_t desc = arg(c, 1), out = arg(c, 2);
    const uint32_t flags = ld(c, desc), max_packets = ld(c, desc + 4), wfx = ld(c, desc + 8);
    // 0x10 is DSSTREAMCAPS_CTRL3D. XACT also sets 0x20000000 and 0x00400000,
    // which emulator sources name DSSTREAMCAPS_ACCURATENOTIFY and NOMERGE (not
    // confirmed against the XDK): how the APU reports and joins packets in
    // hardware. A software mixer reporting each packet as it finishes
    // playing hears the same either way. Any other flag stops the runtime.
    if (flags & ~0x20400010u) unmodelled(fn, strf("stream flags %#x", flags));
    if (!wfx) unmodelled(fn, "a stream without a format");
    auto b = std::make_unique<Buffer>();
    b->stream = true;
    b->flags = flags;
    b->max_packets = max_packets;
    b->callback = ld(c, desc + 12);
    b->context = ld(c, desc + 16);
    b->voice.set_format(read_format(c, wfx, fn));
    b->voice.make_stream();
    b->voice.mix_bins = read_mix_bins(c, ld(c, desc + 20));
    const uint32_t obj = guest_object();
    st(c, obj, kStreamVtable);
    st(c, obj + 4, kStreamInnerVtable);
    ds().buffers[obj] = std::move(b);
    st(c, out, obj);
    ret(c, kDsOk, 16);
}

Buffer& stream(rt::Context& c, uint32_t a, const char* fn) {
    Buffer& b = buffer(c, a, fn);
    if (!b.stream) unmodelled(fn, strf("a stream call on a buffer (%08X)", a));
    return b;
}

void stream_add_ref(rt::Context& c) {
    Lock lock_(ds().m); ret(c, ++stream(c, arg(c, 0), "CDirectSoundStream_AddRef").refs, 4); }

void stream_release(rt::Context& c) {
    Lock lock_(ds().m);
    const uint32_t a = arg(c, 0);
    Buffer& b = stream(c, a, "CDirectSoundStream_Release");
    const uint32_t refs = --b.refs;
    if (refs == 0) {
        catch_up(c);
        if (!b.packets.empty()) unmodelled("CDirectSoundStream_Release", "releasing a stream with packets still attached");
        ds().buffers.erase(a);
    }
    ret(c, refs, 4);
}

// HRESULT GetStatus(LPDWORD pdwStatus), as the original's (0x1CA7AD): READY
// (1) while it takes another packet; a running voice is PAUSED (0x20000)
// when paused or starved, STARVED (0x40000) too when starved, else PLAYING
// (0x10000).
void stream_get_status(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    Buffer& b = stream(c, arg(c, 0), "CDirectSoundStream_GetStatus");
    uint32_t status = 0;
    size_t held = b.packets.size();
    for (const Completion& k : ds().completions) held -= k.stream == arg(c, 0) && b.packets.count(k.id) ? 1 : 0;
    if (held < b.max_packets) status |= 1;
    if (b.voice.playing()) {
        if (b.voice.paused() || b.voice.starved()) status |= 0x20000;
        if (b.voice.starved()) status |= 0x40000;
        if (!(status & 0x20000)) status |= 0x10000;
    }
    st(c, arg(c, 1), status);
    ret(c, kDsOk, 8);
}

// HRESULT Process(LPCXMEDIAPACKET pInputPacket, LPXMEDIAPACKET pOutputPacket):
// XMEDIAPACKET { pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus,
// hCompletionEvent, prtTimestamp, pContext }. The packet is queued behind
// the others, its status pending until it has played.
void stream_process(rt::Context& c) {
    Lock lock_(ds().m);
    const char* fn = "CDirectSoundStream_Process";
    catch_up(c);
    DirectSound& d = ds();
    Buffer& b = stream(c, arg(c, 0), fn);
    const uint32_t pkt = arg(c, 1);
    if (arg(c, 2)) unmodelled(fn, "an output packet");
    // A flush waiting on the envelope's release happens now: the stream goes
    // on with the new sound, as the release would have ended the old one.
    if (b.flush_after_release) {
        b.flush_after_release = false;
        for (const audio::Voice::Done& done : b.voice.flush()) d.completions.push_back({arg(c, 0), done.id, done.bytes, kPacketFlushed});
    }
    // The packets it holds: those not yet completed (a completion waiting to
    // be reported has freed its place, as the APU's has).
    size_t held = b.packets.size();
    for (const Completion& k : d.completions) held -= k.stream == arg(c, 0) && b.packets.count(k.id) ? 1 : 0;
    if (held >= b.max_packets) unmodelled(fn, strf("more than the %u packets the stream takes", b.max_packets));
    const uint32_t data = ld(c, pkt), bytes = ld(c, pkt + 4);
    if (ld(c, pkt + 20)) unmodelled(fn, "a packet with a time stamp");
    const uint32_t block = b.voice.format().block_bytes();
    if (bytes % block) unmodelled(fn, strf("a packet of %u bytes (not whole %u-byte blocks)", bytes, block));
    Buffer::Packet p{ld(c, pkt + 8), ld(c, pkt + 12), ld(c, pkt + 16), ld(c, pkt + 24)};
    if (p.status) st(c, p.status, kPacketPending);
    if (p.event) st(c, p.event + 4, 0);
    const uint32_t id = d.next_packet++;
    b.packets[id] = p;
    b.voice.queue_packet(data, bytes, id);
    if (b.resume_on_start) b.voice.pause(false), b.resume_on_start = false;
    ret(c, kDsOk, 12);
}

// HRESULT SetFormat(LPCWAVEFORMATEX): a new format for the packets to come.
void stream_set_format(rt::Context& c) {
    Lock lock_(ds().m);
    const char* fn = "IDirectSoundStream_SetFormat";
    catch_up(c);
    Buffer& b = stream(c, arg(c, 0), fn);
    if (!b.packets.empty()) unmodelled(fn, "changing the format with packets attached");
    b.voice.set_format(read_format(c, arg(c, 1), fn));
    ret(c, kDsOk, 8);
}

// HRESULT Discontinuity(): the data so far is all there is; the stream stops
// when it runs out, instead of starving.
void stream_discontinuity(rt::Context& c) {
    Lock lock_(ds().m);
    catch_up(c);
    stream(c, arg(c, 0), "CDirectSoundStream_Discontinuity").voice.discontinuity();
    ret(c, kDsOk, 4);
}

// Drops the packets not yet played: each reported flushed, with how much
// of it played (the lock held; the caller delivers after).
void flush_stream(uint32_t a, Buffer& b) {
    b.flush_after_release = false;
    for (const audio::Voice::Done& done : b.voice.flush()) ds().completions.push_back({a, done.id, done.bytes, kPacketFlushed});
}

// HRESULT Flush()
void stream_flush(rt::Context& c) {
    {
        Lock lock_(ds().m);
        catch_up(c);
        const uint32_t a = arg(c, 0);
        flush_stream(a, stream(c, a, "CDirectSoundStream_Flush"));
    }
    deliver(c);
    ret(c, kDsOk, 4);
}

// HRESULT FlushEx(REFERENCE_TIME rtTimeStamp, DWORD dwFlags): as the
// original (0x1CA73B), flags 2 and 4 let the envelope's release play out
// before the packets are flushed (here: the voice released, the flush when
// it has stopped), and 1 returns without waiting. Without an envelope the
// release is instant: a flush now. A time stamp stops the runtime.
void stream_flush_ex(rt::Context& c) {
    const char* fn = "CDirectSoundStream_FlushEx";
    {
        Lock lock_(ds().m);
        catch_up(c);
        const uint32_t a = arg(c, 0), flags = arg(c, 3);
        if (arg(c, 1) || arg(c, 2)) unmodelled(fn, "a flush at a time stamp");
        if (flags & ~7u) unmodelled(fn, strf("flags %#x", flags));
        Buffer& b = stream(c, a, fn);
        if (flags & 6) b.voice.release();
        if ((flags & 6) && b.voice.playing()) b.flush_after_release = true;
        else flush_stream(a, b);
    }
    deliver(c);
    ret(c, kDsOk, 16);
}

// HRESULT Pause(DWORD dwPause): 0 resumes, 1 pauses. 3, as the original's
// voice code (0x1CB290) does it: resume now if the voice is running, else
// when it starts (its first packet). 2 sets another of the voice's flags,
// not modelled yet.
void stream_pause(rt::Context& c) {
    Lock lock_(ds().m);
    const char* fn = "CDirectSoundStream_Pause";
    catch_up(c);
    Buffer& b = stream(c, arg(c, 0), fn);
    const uint32_t how = arg(c, 1);
    if (how == 2 || how > 3) unmodelled(fn, strf("pause mode %u", how));
    b.resume_on_start = false;
    if (how == 3 && !b.voice.playing()) b.resume_on_start = true;
    else b.voice.pause(how == 1);
    ret(c, kDsOk, 8);
}

// HRESULT GetVoiceProperties(LPDSVOICEPROPS): what the APU's voice holds, as
// the original reads it back (CMcpxVoiceClient_GetVoiceProperties,
// 0x1CCB0F): the mix bins in the voice's order, each with the attenuation
// DirectSound programmed for it, the bin's volume plus the voice's volume
// less its headroom, in the hardware's 12-bit 1/64 dB steps (so rounded as
// the hardware holds it), unused entries bin -1 at -10000; the pitch
// register; and five values from a 3D voice's data, zero without 3D.
void voice_properties(rt::Context& c) {
    Lock lock_(ds().m);
    const char* fn = "GetVoiceProperties";
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), fn);
    if (b.flags & 0x10) unmodelled(fn, "the properties of a 3D voice");
    const uint32_t out = arg(c, 1);
    const audio::Voice& v = b.voice;
    std::vector<audio::MixBinVolume> bins = v.mix_bins;
    if (bins.empty()) bins = {{audio::kFrontLeft, 0}, {audio::kFrontRight, 0}};
    if (bins.size() > 8) unmodelled(fn, strf("%zu mix bins", bins.size()));
    st(c, out, uint32_t(bins.size()));
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t bin = 0xFFFFFFFFu;
        int32_t volume = -10000;
        if (i < bins.size()) {
            const int64_t attenuation = -(int64_t(bins[i].volume) + v.volume - v.headroom);  // millibels, >= 0
            const uint32_t hw = std::min<uint32_t>(0xFFF, uint32_t(uint32_t(attenuation) * 64u / 100u));
            bin = bins[i].bin;
            volume = -int32_t((hw * 100u) >> 6);
        }
        st(c, out + 4 + 8 * i, bin);
        st(c, out + 8 + 8 * i, uint32_t(volume));
    }
    st(c, out + 0x44, uint32_t(int32_t(int16_t(v.pitch))));
    for (uint32_t k = 0; k < 5; ++k) st(c, out + 0x48 + 4 * k, 0);
    ret(c, kDsOk, 8);
}

struct DsFn {
    const char* name;
    HleFn fn;
};
constexpr DsFn kDs[] = {
    {"DirectSoundCreate", direct_sound_create},
    {"IDirectSound_Release", ds_release},
    {"IDirectSound_SetMixBinHeadroom", set_mix_bin_headroom},
    {"IDirectSound_CreateSoundBuffer", create_sound_buffer},
    {"IDirectSound_CommitDeferredSettings", commit_deferred_settings},
    {"IDirectSound_DownloadEffectsImage", download_effects_image},
    {"IDirectSound_SetI3DL2Listener", set_i3dl2_listener},
    {"DirectSoundDoWork", do_work},
    {"IDirectSoundBuffer_Release", buffer_release},
    {"IDirectSoundBuffer_SetBufferData", set_buffer_data},
    {"IDirectSoundBuffer_SetFormat", set_format},
    {"IDirectSoundBuffer_SetVolume", set_volume},
    {"IDirectSoundBuffer_SetPitch", set_pitch},
    {"IDirectSoundBuffer_SetHeadroom", set_headroom},
    {"IDirectSoundBuffer_SetMixBins", set_mix_bins},
    {"IDirectSoundBuffer_SetMixBinVolumes_8", set_mix_bin_volumes},
    {"IDirectSoundBuffer_SetLFO", set_lfo},
    {"IDirectSoundBuffer_SetEG", set_eg},
    {"IDirectSoundBuffer_SetFilter", set_filter},
    {"IDirectSoundBuffer_Set3DVoiceData", set_3d_voice_data},
    {"IDirectSoundBuffer_Use3DVoiceData", use_3d_voice_data},
    {"IDirectSoundBuffer_SetLoopRegion", set_loop_region},
    {"IDirectSoundBuffer_SetPlayRegion", set_play_region},
    {"IDirectSoundBuffer_SetCurrentPosition", set_current_position},
    {"IDirectSoundBuffer_SetOutputBuffer", set_output_buffer},
    {"IDirectSoundBuffer_Play", play},
    {"IDirectSoundBuffer_Stop", stop},
    {"IDirectSoundBuffer_StopEx", stop_ex},
    {"IDirectSoundBuffer_GetStatus", get_status},
    {"IDirectSoundBuffer_SetFrequency", set_frequency},
    {"IDirectSoundBuffer_GetCurrentPosition", get_current_position},
    {"IDirectSoundBuffer_Lock", lock_buffer},
    {"IDirectSoundBuffer_Unlock", unlock_buffer},
    {"IDirectSound_CreateSoundStream", create_sound_stream},
    {"CDirectSoundStream_AddRef", stream_add_ref},
    {"CDirectSoundStream_Release", stream_release},
    {"CDirectSoundStream_GetStatus__r2", stream_get_status},
    {"CDirectSoundStream_Process", stream_process},
    {"CDirectSoundStream_Discontinuity", stream_discontinuity},
    {"CDirectSoundStream_Flush", stream_flush},
    // The stream's setters take the same arguments as the buffer's: the
    // C++ methods, and the C interface's thunks that jump to them.
    {"CDirectSoundStream_SetVolume", set_volume},
    {"IDirectSoundStream_SetVolume", set_volume},
    {"CDirectSoundStream_SetPitch", set_pitch},
    {"IDirectSoundStream_SetPitch", set_pitch},
    {"CDirectSoundStream_SetHeadroom", set_headroom},
    {"IDirectSoundStream_SetHeadroom", set_headroom},
    {"CDirectSoundStream_SetMixBins", set_mix_bins},
    {"IDirectSoundStream_SetMixBins", set_mix_bins},
    {"CDirectSoundStream_SetMixBinVolumes_8", set_mix_bin_volumes},
    {"IDirectSoundStream_SetMixBinVolumes_8", set_mix_bin_volumes},
    {"CDirectSoundStream_SetLFO", set_lfo},
    {"IDirectSoundStream_SetLFO", set_lfo},
    {"CDirectSoundStream_SetEG", set_eg},
    {"IDirectSoundStream_SetEG", set_eg},
    {"CDirectSoundStream_SetFilter", set_filter},
    {"IDirectSoundStream_SetFilter", set_filter},
    {"CDirectSoundStream_Set3DVoiceData", set_3d_voice_data},
    {"IDirectSoundStream_Set3DVoiceData", set_3d_voice_data},
    {"CDirectSoundStream_Use3DVoiceData", use_3d_voice_data},
    {"IDirectSoundStream_Use3DVoiceData", use_3d_voice_data},
    {"CDirectSoundStream_SetOutputBuffer", set_output_buffer},
    {"IDirectSoundStream_SetOutputBuffer", set_output_buffer},
    {"CDirectSoundStream_Pause", stream_pause},
    {"IDirectSoundStream_Pause", stream_pause},
    {"CDirectSoundStream_FlushEx", stream_flush_ex},
    {"CDirectSoundStream_GetVoiceProperties", voice_properties},
    {"CDirectSoundBuffer_GetVoiceProperties", voice_properties},
    {"IDirectSoundBuffer_GetVoiceProperties", voice_properties},
    {"IDirectSoundStream_GetVoiceProperties", voice_properties},
    {"CDirectSoundStream_SetFormat", stream_set_format},
    {"IDirectSoundStream_SetFormat", stream_set_format},
    {"IDirectSoundStream_FlushEx", stream_flush_ex},
};

}  // namespace

void pump_audio(rt::Context& c) {
    {
        Lock lock_(ds().m);
        catch_up(c);
    }
    deliver(c);
}

void register_dsound(const std::string& dump_wav, platform::AudioOut* out) {
    ds().dump_path = dump_wav;
    ds().out = out;
    add_idle_hook(pump_audio);
    register_mmio_read(0xFE80200C, "APU global sample counter", apu_global_sample_count);
    for (const DsFn& f : kDs) {
        const XdkSymbol* s = xdk_symbol(f.name);
        if (!s) {
            std::fprintf(stderr, "qilin: DirectSound replacement %s has no address in this XBE\n", f.name);
            std::abort();
        }
        register_hle(s->address, f.name, f.fn);
    }
}

}  // namespace qilin::host
