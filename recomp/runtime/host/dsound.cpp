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
#include "audio_out.hpp"
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
};

// A packet the mixer finished (or a flush dropped), waiting to be reported
// where the console would report it: in a DPC, so not while the processor's
// level is raised to DISPATCH_LEVEL or above.
struct Completion {
    uint32_t stream, id, bytes, status;
};

struct DirectSound {
    uint32_t object = 0, refs = 0;
    std::map<uint32_t, std::unique_ptr<Buffer>> buffers;  // by guest address
    std::chrono::steady_clock::time_point epoch;
    platform::AudioOut* out = nullptr;  // the speakers, if there's a sound device
    uint64_t mixed = 0;  // output frames mixed so far
    uint32_t next_packet = 1;
    std::vector<Completion> completions;
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
        for (auto& [addr, b] : d.buffers)
            if (b->stream)
                for (const audio::Voice::Done& done : b->voice.take_completed()) d.completions.push_back({addr, done.id, done.bytes, kPacketSuccess});
        if (d.wav) d.wav->write(out.data(), n);
        if (d.out) d.out->push(out.data(), n);
        d.mixed += n;
    }
}

// Reports finished packets, as the console's DPC does: the completed size
// and status into the packet's words, its event signalled, the stream's
// callback called (pStreamContext, pPacketContext, dwStatus). Only below
// DISPATCH_LEVEL, as a DPC would wait for the level to drop.
void deliver(rt::Context& c) {
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
        if (pk.event) st(c, pk.event + 4, 1);  // the KEVENT's SignalState (nothing waits on it without threads)
        if (b.callback) call_guest(c, b.callback, {b.context, pk.context, k.status});
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
    deliver(c);
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
    if (flags & 0x10) notice_once("3D positioning (DSSTREAMCAPS_CTRL3D streams play unpositioned)");
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

void stream_add_ref(rt::Context& c) { ret(c, ++stream(c, arg(c, 0), "CDirectSoundStream_AddRef").refs, 4); }

void stream_release(rt::Context& c) {
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
    catch_up(c);
    Buffer& b = stream(c, arg(c, 0), "CDirectSoundStream_GetStatus");
    uint32_t status = 0;
    if (b.packets.size() < b.max_packets) status |= 1;
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
    const char* fn = "CDirectSoundStream_Process";
    catch_up(c);
    DirectSound& d = ds();
    Buffer& b = stream(c, arg(c, 0), fn);
    const uint32_t pkt = arg(c, 1);
    if (arg(c, 2)) unmodelled(fn, "an output packet");
    if (b.packets.size() >= b.max_packets) unmodelled(fn, strf("more than the %u packets the stream takes", b.max_packets));
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
    catch_up(c);
    stream(c, arg(c, 0), "CDirectSoundStream_Discontinuity").voice.discontinuity();
    ret(c, kDsOk, 4);
}

// Drops the packets not yet played: each reported flushed, with how much of it played.
void flush_stream(rt::Context& c, uint32_t a, Buffer& b) {
    for (const audio::Voice::Done& done : b.voice.flush()) ds().completions.push_back({a, done.id, done.bytes, kPacketFlushed});
    deliver(c);
}

// HRESULT Flush()
void stream_flush(rt::Context& c) {
    catch_up(c);
    const uint32_t a = arg(c, 0);
    flush_stream(c, a, stream(c, a, "CDirectSoundStream_Flush"));
    ret(c, kDsOk, 4);
}

// HRESULT FlushEx(REFERENCE_TIME rtTimeStamp, DWORD dwFlags): as the
// original (0x1CA73B), flags 2 and 4 let the envelope's release play out
// before the packets are flushed, and 1 returns without waiting. Without
// envelopes the release is instant, so each is a flush now. A time stamp
// stops the runtime.
void stream_flush_ex(rt::Context& c) {
    const char* fn = "CDirectSoundStream_FlushEx";
    catch_up(c);
    const uint32_t a = arg(c, 0), flags = arg(c, 3);
    if (arg(c, 1) || arg(c, 2)) unmodelled(fn, "a flush at a time stamp");
    if (flags & ~7u) unmodelled(fn, strf("flags %#x", flags));
    if (flags & 6) notice_once("envelopes (attack, decay, release)");
    flush_stream(c, a, stream(c, a, fn));
    ret(c, kDsOk, 16);
}

// HRESULT Pause(DWORD dwPause): 0 resumes, 1 pauses. 3, as the original's
// voice code (0x1CB290) does it: resume now if the voice is running, else
// when it starts (its first packet). 2 sets another of the voice's flags,
// not modelled yet.
void stream_pause(rt::Context& c) {
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
    catch_up(c);
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
