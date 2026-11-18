# Audio: XACT lifted, DirectSound native

Last updated 2026-11-18 (v0.15.0: XACT lifted, DirectSound replaced over a software mixer).

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
- **Tests** (`qilin_audio_tests`, CI): PCM playback, end of data, volume, looping, half-rate playback with interpolation, mix bins, the ADPCM decoder, ADPCM sizes.

## Not applied yet

These are accepted and recorded, and `qilin_run` names each once when the game first uses it:

- **Effects:** the game downloads the standard effects image (18,012 bytes) with the I3DL2 reverb as effect 0 and the crosstalk canceller as effect 1. The DSP isn't emulated: the reverb will be a native I3DL2 reverb driven by the same `SetI3DL2Listener` parameters and the effect-send mix bins.
- **Envelopes, LFOs, voice filters** (`SetEG`, `SetLFO`, `SetFilter`, and `StopEx` with the envelope's release).
- **3D positioning** (`Set3DVoiceData`, `Use3DVoiceData`, HRTF): 3D voices play unpositioned.
- **The LFE bin and headroom** (per voice and per mix bin: on the APU, voice headroom attenuates before mixing and bin headroom restores it, so ignoring both keeps the overall level).

Not modelled, and stopping the runtime if reached: submix buffers (`SetOutputBuffer`, `DSBCAPS_MIXIN`), effect-input buffers, buffers with their own memory, `DSBPLAY_SYNCHPLAYBACK`, stopping at a time stamp, streams (`CreateSoundStream`: streamed wave banks and music, still to come), and WMA decoding (WMADEC, for music).

## Surround (planned)

The Xbox mixed in 5.1 natively, and so does this engine: every voice already goes to the six speaker bins (front left, front right, centre, LFE, back left, back right). Only the last step folds them to stereo. The plan:

1. **3D positioning**, which decides a positioned sound's speaker volumes: DirectSound's 3D calculator (`CDirectSound3DCalculator`, and the full-HRTF path the game turns on with `DirectSoundUseFullHRTF`) looks like pure computation, so it would be lifted, like `XAudioCalculatePitch`, giving the console's exact panning. To confirm when it's reached.
2. **A speaker setting** (stereo, 5.1, and 7.1 with the back channels copied to the sides), reported to the game as the console's audio setting (`ExQueryNonVolatileSetting`'s audio flags, Stereo today), and used as the mixer's output layout: the six bins go to SDL3 as six channels, LFE included, and the OS carries them to HDMI, optical or a surround headset. The Xbox's real-time Dolby Digital encoder (the `DOLBY` section) existed because its optical output could only carry surround that way; it isn't needed.
3. **The game does the rest**, as on a console set to surround: it and XACT send to the back speakers themselves.

## Output

There's no audio device yet: the mix goes to `--dump-audio` or nowhere. The plan is SDL3 for the window, controller and audio output (the user's controller is an Xbox One pad over Bluetooth, which SDL3 supports with rumble on every target). Output then pulls from the mixer on SDL's audio thread, which is the roadmap's "audio mixing off the game thread".
