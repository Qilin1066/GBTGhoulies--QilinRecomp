# Audio: XACT lifted, DirectSound native

Last updated 2026-11-18 (v0.25.0: sound output through SDL3, DirectSound streams; v0.15.0: XACT lifted, DirectSound replaced over a software mixer).

The game plays its sound through XACT, Microsoft's audio engine (cues, sound banks, wave banks, notifications), which sits on DirectSound, which programs the Xbox's audio chip (the APU: a voice processor that plays and mixes up to 256 voices, and a DSP that runs effects such as reverb).

```
lifted game code  →  XACT (lifted)  →  DirectSound (native, dsound.cpp)  →  audio engine (audio/)  →  output
```

**XACT is lifted**, like the C runtime and XAPI: its cue logic, variation choices, volume curves and notifications are the original's, exactly. It touches the hardware once: it reads the APU's global sample counter (`0xFE80200C`) as its clock. The lifter turns that into a device-register read (`recompiler-core.md`), and the runtime answers it from the mixer's clock, so XACT's idea of time and the sound it hears agree. Its differential test: 332 functions, 0 mismatches.

**DirectSound is native** (`recomp/runtime/host/dsound.cpp`): it's the layer that drives the APU's registers (289 register accesses in its code), and the APU isn't emulated. Its functions are replaced at the API level, each at its address in the XBE with its original argument size. Three of them are pure computation and are lifted instead, so their values are the original's: `XAudioCalculatePitch` and the PCM and Xbox ADPCM format builders.

## The audio engine

`recomp/runtime/audio/` knows nothing about DirectSound; it has voices and a mixer.

- **Voices** play sample data straight from guest memory (DirectSound's buffers point into it): 8- and 16-bit PCM and Xbox ADPCM, mono or stereo, at any rate.
- **Xbox ADPCM:** IMA ADPCM in 36-byte blocks per channel (a 4-byte header with the predictor and step index, then 32 bytes of nibbles in 4-byte groups interleaved across channels). A block is 64 samples, as DirectSound counts them; the header's sample seeds the predictor. *To check against the hardware:* some decoders also output the header sample, giving 65 samples per block; if xemu recordings show sounds ending 1.5% late, that's the difference.
- **Pitch and volume** in the Xbox's units: pitch in 1/4096 octave relative to 48 kHz (a voice's format sets it), volume in hundredths of a decibel. Resampling is linear interpolation.
- **Mix bins:** each channel goes to the speaker bins it names (front left/right/centre, back left/right) at their volumes, folded down to stereo: centre at −3 dB to both sides, back bins to their side.
- **Regions:** play regions, loop regions, looping, the play position, stopping at the end of a non-looping region.
- **The mix** is 48 kHz stereo, the APU's rate. Its clock is the host's: on every DirectSound call (and `DirectSoundDoWork`, which the game calls each frame) the mixer catches up to the time since `DirectSoundCreate`, so a sound plays as long as on the console and `GetStatus` reports its end when it comes. `qilin_run --dump-audio FILE.wav` writes everything mixed to a WAV file.
- **Submixes** (v0.21.0): a `DSBCAPS_MIXIN` buffer is a submix voice with no sample data. Voices sent to it with `SetOutputBuffer` mix into its input (channel for channel; a mono submix takes the sum) at their own volume, and it plays that input with its own volume and mix bins. One level: a submix feeding another stops the runtime.
- **3D positioning's maths is lifted** (v0.21.0): DirectSound's 3D calculator (`IDirectSound3DCalculator_Calculate3D` and `GetVoiceData`, which XACT calls), the full-HRTF table `DirectSoundUseFullHRTF` installs, and DirectSound's four start-up initializers, 41 functions. The voice settings it produces (`Set3DVoiceData`) are recorded but not applied yet.
- **Streams** (v0.25.0, `CreateSoundStream`): XACT plays its wave banks through DirectSound streams. A stream is a voice that plays packets (XMEDIAPACKET) of guest memory one after another as one signal, the interpolation running across their edges. With no packet left it's starved: silent, still running, and the next packet carries on; after `Discontinuity` it stops instead when the packets run out. The stream object is the original's shape where callers look (its first words are CDirectSoundStream's vtables, `0x20A038` and `0x20A028`), so XACT's calls through the vtable (`Process`, `GetStatus`, `Discontinuity`, `Flush`) reach the replacements at those methods' addresses; the setters are the buffer's. A finished packet is reported as the console's DPC reports it, and only where one could run, below DISPATCH_LEVEL (`DirectSoundDoWork`, `Swap`, the game's waits): the completed size and status (`S_OK`, or `E_ABORT` when flushed) into the packet's words, its event signalled, the stream's callback called. `GetStatus`, `Pause` (including mode 3, resume when the voice starts) and `FlushEx` follow the original's code (`0x1CA7AD`, `0x1CB290`, `0x1CA73B`); without envelopes a flush with a release is a flush now. `GetVoiceProperties` (streams and buffers) returns what the original reads back from the APU's voice: each bin's attenuation as DirectSound programs it (the bin's volume plus the voice's, less the headroom, in the hardware's 12-bit 1/64 dB steps), unused entries bin −1 at −10000, the pitch, zero 3D values; a 3D voice's properties stop the runtime. Stream flags `0x20000000` and `0x00400000` (which emulator sources call ACCURATENOTIFY and NOMERGE; not confirmed) change how the APU's hardware handles packets, not what's heard, and are accepted. Pause mode 2, time stamps, output packets and a format change with packets attached stop the runtime.
- **Tests** (`qilin_audio_tests`, CI): PCM playback, end of data, volume, looping, half-rate playback with interpolation, mix bins, the ADPCM decoder, ADPCM sizes.

## Not applied yet

These are accepted and recorded, and `qilin_run` names each once when the game first uses it:

- **Effects:** the game downloads the standard effects image (18,012 bytes) with the I3DL2 reverb as effect 0 and the crosstalk canceller as effect 1. The DSP isn't emulated: the reverb will be a native I3DL2 reverb driven by the same `SetI3DL2Listener` parameters and the effect-send mix bins.
- **Envelopes, LFOs, voice filters** (`SetEG`, `SetLFO`, `SetFilter`, and `StopEx` with the envelope's release).
- **3D positioning** (`Set3DVoiceData`, `Use3DVoiceData`, HRTF): 3D voices play unpositioned.
- **The LFE bin and headroom** (per voice and per mix bin: on the APU, voice headroom attenuates before mixing and bin headroom restores it, so ignoring both keeps the overall level).

Not modelled, and stopping the runtime if reached: submixes of submixes, effect-input buffers, buffers with their own memory, `DSBPLAY_SYNCHPLAYBACK`, stopping at a time stamp, and WMA decoding (WMADEC, for music, if the game uses it).

## Surround (planned)

The Xbox mixed in 5.1 natively, and so does this engine: every voice already goes to the six speaker bins (front left, front right, centre, LFE, back left, back right). Only the last step folds them to stereo. The plan:

1. **3D positioning**, which decides a positioned sound's speaker volumes: DirectSound's 3D calculator (`CDirectSound3DCalculator`, and the full-HRTF path the game turns on with `DirectSoundUseFullHRTF`) is pure computation and lifted (v0.21.0), so the panning is the console's; what remains is applying the voice data it produces.
2. **A speaker setting** (stereo, 5.1, and 7.1 with the back channels copied to the sides), reported to the game as the console's audio setting (`ExQueryNonVolatileSetting`'s audio flags, Stereo today), and used as the mixer's output layout: the six bins go to SDL3 as six channels, LFE included, and the OS carries them to HDMI, optical or a surround headset. The Xbox's real-time Dolby Digital encoder (the `DOLBY` section) existed because its optical output could only carry surround that way; it isn't needed.
3. **The game does the rest**, as on a console set to surround: it and XACT send to the back speakers themselves.

## Output

Since v0.25.0 the mix plays through SDL3 (`recomp/runtime/platform/audio_out.*`) on the default playback device, 48 kHz stereo floats. The mixer still runs on the audio clock (the host's steady clock), catching up on DirectSound calls, at each `Swap`, and in the game's waits; what it mixes is queued to the device. The queue starts with 60 ms of silence, so the device never runs dry between catch-ups, and is held there by trimming the playback rate a little (at most 0.5%, inaudible), which absorbs the drift between the sound card's clock and the host's. After a stall, frames older than the queue's limit (four times the cushion) are dropped, as they'd have played already on the console. The device buffer is asked to be 512 frames. Measured through SDL's disk driver, what reaches the device is the mix, about 0.2 s behind it there (that driver buffers more than a sound card does). `--mute` (and `--headless`) play nothing; `--dump-audio` still writes the mix.

Next: mixing on SDL's audio thread instead of the game's (the roadmap's "audio mixing off the game thread"), then surround (above).
