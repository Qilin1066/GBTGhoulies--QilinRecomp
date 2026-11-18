// DirectSound (the XDK's DSOUND library), replaced natively over the audio
// engine (recomp/runtime/audio). docs/audio.md describes the design.
//
// XACT, lifted, calls these; so does the game, once. DirectSound on the Xbox
// programs the APU's voice processor and runs effects on its DSP; here the
// voices are the audio engine's, mixed in software. The audio clock is the
// host's: on every DirectSound call the mixer catches up to the time since
// the device was created, so a sound plays for as long as it does on the
// console and GetStatus reports its end when it comes.
//
// Settings that change a voice's sound but aren't applied yet (envelopes,
// LFOs, filters, effect sends, 3D) are recorded and named once at run time.
// Anything not modelled at all stops the runtime with its name.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio.hpp"
#include "host.hpp"
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

// ---------------------------------------------------------------- objects
struct Buffer {
    uint32_t refs = 1;
    uint32_t flags = 0;
    uint32_t output = 0;  // the submix buffer it feeds (SetOutputBuffer), or 0 for the mix bins
    audio::Voice voice;
};

struct DirectSound {
    uint32_t object = 0, refs = 0;
    std::map<uint32_t, std::unique_ptr<Buffer>> buffers;  // by guest address
    std::chrono::steady_clock::time_point epoch;
    uint64_t mixed = 0;  // output frames mixed so far
    std::unique_ptr<audio::WavWriter> wav;
    std::string dump_path;
};
DirectSound& ds() {
    static DirectSound d;
    return d;
}

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

// Brings the mix up to now.
void catch_up(rt::Context& c) {
    DirectSound& d = ds();
    if (!d.object) return;
    const uint64_t target = audio_now();
    std::vector<float> out;
    while (d.mixed < target) {
        const uint32_t n = uint32_t(std::min<uint64_t>(target - d.mixed, 1024));
        out.assign(size_t(n) * 2, 0.0f);
        // Submixes first: clear their inputs and mix their sources into them.
        for (auto& [addr, b] : d.buffers)
            if (b->voice.submix()) b->voice.clear_input(n);
        for (auto& [addr, b] : d.buffers)
            if (b->output) b->voice.mix_into(c.mem, c.mem_size, d.buffers.at(b->output)->voice, n);
        for (auto& [addr, b] : d.buffers)
            if (!b->output) b->voice.mix(c.mem, c.mem_size, out.data(), n);
        if (d.wav) d.wav->write(out.data(), n);
        d.mixed += n;
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
        if (bin > audio::kBackRight) notice_once("mixing into effect sends (mix bins after BackRight)");
        if (bin == audio::kLowFrequency) notice_once("the low-frequency (LFE) mix bin");
    }
    return v;
}

// ---------------------------------------------------------------- DirectSound
// HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND*, LPUNKNOWN): the one
// device; a second create returns it with another reference, as on the Xbox.
void direct_sound_create(rt::Context& c) {
    DirectSound& d = ds();
    if (!d.object) {
        d.object = guest_object();
        d.epoch = std::chrono::steady_clock::now();
        if (!d.dump_path.empty()) {
            d.wav = std::make_unique<audio::WavWriter>(d.dump_path);
            if (!d.wav->ok()) std::fprintf(stderr, "qilin: can't write %s\n", d.dump_path.c_str());
        }
        std::printf("qilin_run: DirectSound: %u Hz stereo software mix%s%s\n", audio::kOutputRate,
                    d.wav ? ", written to " : "", d.wav ? d.dump_path.c_str() : "");
    }
    ++d.refs;
    st(c, arg(c, 1), d.object);
    ret(c, kDsOk, 12);
}

void ds_release(rt::Context& c) {
    DirectSound& d = ds();
    if (arg(c, 0) != d.object || d.refs == 0) unmodelled("IDirectSound_Release", "releasing an object that isn't the device");
    --d.refs;  // the device stays: freeing it isn't modelled, and a title creates it once
    ret(c, d.refs, 4);
}

// HRESULT IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND, DWORD dwMixBin, DWORD dwHeadroom):
// the bin's headroom (its attenuation before the speakers), recorded.
void set_mix_bin_headroom(rt::Context& c) { ret(c, kDsOk, 12); }

// HRESULT IDirectSound_CreateSoundBuffer(LPDIRECTSOUND, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN)
// DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin.
void create_sound_buffer(rt::Context& c) {
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
    if (flags & 0x10) notice_once("3D positioning (DSBCAPS_CTRL3D buffers play unpositioned)");
    const uint32_t obj = guest_object();
    ds().buffers[obj] = std::move(b);
    st(c, out, obj);
    ret(c, kDsOk, 16);
}

void buffer_release(rt::Context& c) {
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
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_SetBufferData");
    if (b.voice.submix()) unmodelled("IDirectSoundBuffer_SetBufferData", "sample data for a submix buffer");
    b.voice.stop();
    b.voice.set_data(arg(c, 1), arg(c, 2));
    ret(c, kDsOk, 12);
}

// HRESULT IDirectSoundBuffer_SetFormat(LPDIRECTSOUNDBUFFER, LPCWAVEFORMATEX)
void set_format(rt::Context& c) {
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_SetFormat");
    if (b.voice.submix()) unmodelled("IDirectSoundBuffer_SetFormat", "a submix buffer's format");
    b.voice.set_format(read_format(c, arg(c, 1), "IDirectSoundBuffer_SetFormat"));
    ret(c, kDsOk, 8);
}

#define BUFFER_SETTER(name, bytes, body)                \
    void name(rt::Context& c) {                         \
        catch_up(c);                                    \
        Buffer& b = buffer(c, arg(c, 0), #name);        \
        (void)b;                                        \
        body;                                           \
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
BUFFER_SETTER(set_eg, 8, notice_once("envelopes (attack, decay, release)"))
BUFFER_SETTER(set_filter, 8, notice_once("voice filters"))
BUFFER_SETTER(set_3d_voice_data, 8, notice_once("3D positioning"))
BUFFER_SETTER(use_3d_voice_data, 8, notice_once("3D positioning"))
BUFFER_SETTER(set_loop_region, 12, b.voice.set_loop_region(arg(c, 1), arg(c, 2)))
BUFFER_SETTER(set_play_region, 12, b.voice.set_play_region(arg(c, 1), arg(c, 2)))
BUFFER_SETTER(set_current_position, 8, b.voice.set_position(arg(c, 1)))
BUFFER_SETTER(stop, 4, b.voice.stop())

// HRESULT IDirectSoundBuffer_SetOutputBuffer(LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER pOutputBuffer):
// the voice feeds a submix buffer (DSBCAPS_MIXIN) instead of the mix bins;
// null sends it back to the mix bins (the default).
void set_output_buffer(rt::Context& c) {
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
// envelopes, at once), DSBSTOPEX_RELEASEWAVEFORM (2: play on past the loop to the end).
void stop_ex(rt::Context& c) {
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_StopEx");
    if (arg(c, 1) || arg(c, 2)) unmodelled("IDirectSoundBuffer_StopEx", "stopping at a time stamp");
    const uint32_t flags = arg(c, 3);
    if (flags & 2) {
        if (b.voice.playing()) b.voice.play(false);  // leave the loop, finish the region
    } else {
        if (flags & 1) notice_once("envelopes (attack, decay, release)");
        b.voice.stop();
    }
    ret(c, kDsOk, 16);
}

// HRESULT IDirectSoundBuffer_GetStatus(LPDIRECTSOUNDBUFFER, LPDWORD pdwStatus):
// DSBSTATUS_PLAYING (1), DSBSTATUS_LOOPING (4).
void get_status(rt::Context& c) {
    catch_up(c);
    Buffer& b = buffer(c, arg(c, 0), "IDirectSoundBuffer_GetStatus");
    st(c, arg(c, 1), b.voice.playing() ? (b.voice.looping() ? 5u : 1u) : 0u);
    ret(c, kDsOk, 8);
}

// HRESULT IDirectSound_CommitDeferredSettings(LPDIRECTSOUND): settings take effect at once here.
void commit_deferred_settings(rt::Context& c) {
    catch_up(c);
    ret(c, kDsOk, 4);
}

// VOID DirectSoundDoWork(): DirectSound's periodic work; here, mixing up to now.
void do_work(rt::Context& c) {
    catch_up(c);
    ret(c, c.eax, 0);
}

// HRESULT IDirectSound_DownloadEffectsImage(LPDIRECTSOUND, LPCVOID pvImageBuffer,
//     DWORD dwImageSize, LPCDSEFFECTIMAGELOC pImageLoc, LPDSEFFECTIMAGEDESC* ppImageDesc):
// loads a program for the APU's DSP. The game's image holds the standard
// effects; DSEFFECTIMAGELOC says where the I3DL2 reverb and the crosstalk
// canceller are in it. The DSP isn't emulated: those effects will be native
// ones, driven by the same settings (SetI3DL2Listener, the effect mix bins).
void download_effects_image(rt::Context& c) {
    const uint32_t size = arg(c, 2), loc = arg(c, 3), desc_out = arg(c, 4);
    if (desc_out) unmodelled("IDirectSound_DownloadEffectsImage", "returning the image's effect descriptions (ppImageDesc)");
    std::printf("qilin_run: DirectSound: effects image of %u bytes (I3DL2 reverb at effect %d, crosstalk at %d)\n", size,
                loc ? int32_t(ld(c, loc)) : -1, loc ? int32_t(ld(c, loc + 4)) : -1);
    notice_once("the DSP effects (I3DL2 reverb)");
    ret(c, kDsOk, 20);
}

// HRESULT IDirectSound_SetI3DL2Listener(LPDIRECTSOUND, LPCDSI3DL2LISTENER, DWORD dwApply):
// the environmental reverb's parameters, recorded.
void set_i3dl2_listener(rt::Context& c) {
    catch_up(c);
    notice_once("the DSP effects (I3DL2 reverb)");
    ret(c, kDsOk, 12);
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
};

}  // namespace

void register_dsound(const std::string& dump_wav) {
    ds().dump_path = dump_wav;
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
